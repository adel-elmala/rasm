#include "rasm/core/engine.h"
#include "rasm/core/mesh.h"
#include "rasm/core/material.h"
#include "rasm/core/light.h"
#include "rasm/core/transform.h"
#include "rasm/core/entity.h"
#include "rasm/core/camera.h"
#include "rasm/core/renderGraph.h"
#include "rasm/core/utils.h"
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
    ShaderCompiler::Target getShaderCompilerTarget(rasm::Backend backend)
    {
        switch (backend)
        {
        case Backend::VULKAN:
            return ShaderCompiler::Target::SPIRV;
        case Backend::DX12:
            return ShaderCompiler::Target::HLSL;
        case Backend::METAL:
            return ShaderCompiler::Target::MSL;
        default:
            spdlog::error("Unsupported backend for shader compilation.");
            return ShaderCompiler::Target::SPIRV; // Default to SPIR-V
        }
    }

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
            shaderDataBufferDesc.type = ResourceType::BUFFER;
            shaderDataBufferDesc.name = "ShaderDataBuffer for Frame " + std::to_string(i);
            shaderDataBufferDesc.buffer.size = 1024 * 1024; // 1 MB
            shaderDataBufferDesc.buffer.usage = BufferUsage::DEVICE_ADDRESS;

            frameResources[i].shaderDataBuffer = ctx.createBuffer(shaderDataBufferDesc);

            // Create a depth texture.
            auto depthTextureDesc = ResourceDesc{};
            depthTextureDesc.type = ResourceType::TEXTURE;
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

        auto shaderTarget = getShaderCompilerTarget(config.preferredBackend);
        if (!shaderCompiler.initialize(shaderTarget))
        {
            spdlog::error("Failed to initialize shader compiler.");
            isRunning = false;
        }

        // Create bindless texture descriptor set layout, pool, and set
        ResourceDesc bindlessLayoutDesc{};
        bindlessLayoutDesc.type = ResourceType::BINDLESS_DESCRIPTOR_SET_LAYOUT;
        bindlessLayoutDesc.name = "BindlessDescriptorSetLayout";
        bindlessLayoutDesc.bindlessDescriptorSetLayout.stage = ShaderType::FRAGMENT;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.count = MAX_BINDLESS_TEXTURES;
        bindlessLayoutDesc.bindlessDescriptorSetLayout.type = ResourceType::TEXTURE;

        bindlessDescriptorSetLayout = ctx.createBindlessDescriptorSetLayout(bindlessLayoutDesc);

        ResourceDesc bindlessPoolDesc{};
        bindlessPoolDesc.type = ResourceType::DESCRIPTOR_POOL;
        bindlessPoolDesc.name = "BindlessDescriptorPool";
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorType = ResourceType::TEXTURE;
        bindlessPoolDesc.descriptorPool.descriptorType[0].descriptorCount = MAX_BINDLESS_TEXTURES;
        bindlessPoolDesc.descriptorPool.maxSets = 1;

        bindlessDescriptorPool = ctx.createDescriptorPool(bindlessPoolDesc);

        ResourceDesc bindlessSetDesc{};
        bindlessSetDesc.type = ResourceType::DESCRIPTOR_SET;
        bindlessSetDesc.name = "BindlessDescriptorSet";
        bindlessSetDesc.descriptorSet.pool = bindlessDescriptorPool;
        bindlessSetDesc.descriptorSet.layout = bindlessDescriptorSetLayout;

        bindlessDescriptorSet = ctx.allocateDescriptorSet(bindlessSetDesc, bindlessDescriptorPool, bindlessDescriptorSetLayout);
    }

    Engine::~Engine()
    {
        // Clean up resources, free memory, etc.
        for (auto &[handle, tex] : textureData)
        {
            stbi_image_free(tex.data);
        }

        for (auto mesh : registery.meshes)
        {
            if (mesh.type == Mesh::MeshType::GLTF)
            {
                // tinygltf::Model doesn't require explicit cleanup.
            }

            if (mesh.type == Mesh::MeshType::OBJ)
            {
                // If we had implemented OBJ loading, we would clean up any allocated resources here.
            }
        }

        ctx.cleanup();

        this->window.destroyWindow(this->mainWindow);
        spdlog::info("Engine shutdown, cleaned up resources.");
    }

    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        auto handle = getNextMeshHandle();

        ObjRaw objRaw{};
        objRaw.vertices = std::move(vertices);
        objRaw.indices = std::move(indices);

        Mesh mesh{};
        mesh.type = Mesh::MeshType::OBJ;
        mesh.data = std::move(objRaw);

        registery.meshes[handle.index] = std::move(mesh);

        return handle;
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
            isRunning = false;
            return MeshHandle{};
        }

        Mesh mesh{};
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

            mesh.type = Mesh::MeshType::GLTF;
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

            mesh.type = Mesh::MeshType::OBJ;

            ObjRaw objRaw{};
            objRaw.vertices = std::move(out_vertices);
            objRaw.indices = std::move(out_indices);
            mesh.data = std::move(objRaw);
        }

        auto handle = getNextMeshHandle();
        loadedMeshes[path] = handle;

        registery.meshes[handle.index] = std::move(mesh);

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
            isRunning = false;
            return TextureHandle{};
        }

        int width, height, nChannels;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &nChannels, 4);

        if (!data)
        {
            spdlog::error("Failed to load texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        TextureRaw tex{};
        tex.width = width;
        tex.height = height;
        tex.channels = nChannels;
        tex.data = data;

        auto textureDesc = ResourceDesc{};
        textureDesc.name = path;
        textureDesc.type = ResourceType::TEXTURE;
        textureDesc.texture.width = width;
        textureDesc.texture.height = height;
        textureDesc.texture.format = Format::R8G8B8A8_UNORM;
        textureDesc.texture.usage = TextureUsage::SAMPLED;

        auto textureHandle = ctx.createTexture(textureDesc);
        if (!ctx.fillTexture(textureHandle, data))
        {
            spdlog::error("Failed to fill texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        // using the handle index as the slot for bindless descriptor set
        // this will result in a fragmented bindless descriptor set if the handles are not contiguous, (e.g, some handles are not for a sampled texture...),
        // but for simplicity we will use this approach for now
        if (!ctx.updateBindlessDescriptorSet(bindlessDescriptorSet, textureHandle, static_cast<uint32_t>(textureHandle.index)))
        {
            spdlog::error("Failed to update bindless descriptor set for texture: {}", path);
            isRunning = false;
            return TextureHandle{};
        }

        loadedTextures[path] = textureHandle;
        textureData.insert({textureHandle, std::move(tex)});

        return textureHandle;
    }

    BufferHandle Engine::uploadMesh(const MeshHandle &handle)
    {
        // TODO: remove mesh from meshData after uploading to GPU.
        if (handle.index == 0 || handle.index > MAX_MESHES)
        {
            spdlog::error("Mesh handle not found for upload.");
            isRunning = false;
            return BufferHandle{};
        }

        auto mesh = registery.meshes[handle.index];

        if (mesh.type == Mesh::MeshType::GLTF)
        {
            // In a real implementation, this is where we'd upload the GLTF mesh data to the GPU.
            spdlog::info("Uploading GLTF mesh with handle: {}", handle.index);
        }
        else if (mesh.type == Mesh::MeshType::OBJ)
        {
            auto &objRaw = std::get<ObjRaw>(mesh.data);

            auto bufferDesc = ResourceDesc{};
            bufferDesc.name = "OBJ Vertex + index Buffer";
            bufferDesc.type = ResourceType::BUFFER;
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
            isRunning = false;
            return BufferHandle{};
        }

        return BufferHandle{};
    }

    ResourceHandle Engine::createResource(const ResourceDesc &desc)
    {
        ResourceHandle handle{};

        switch (desc.type)
        {
        case ResourceType::TEXTURE:
        {
            auto textureHandle = ctx.createTexture(desc);
            handle.generation = textureHandle.generation;
            handle.index = textureHandle.index;
            break;
        }
        case ResourceType::BUFFER:
        {
            auto bufferHandle = ctx.createBuffer(desc);
            handle.generation = bufferHandle.generation;
            handle.index = bufferHandle.index;
            break;
        }
        case ResourceType::RENDER_TARGET:
        {
            auto renderTargetHandle = ctx.createRenderTarget(desc.name, desc.renderTarget.width, desc.renderTarget.height, desc.renderTarget.colorFormat, desc.renderTarget.depthFormat);
            handle.generation = renderTargetHandle.generation;
            handle.index = renderTargetHandle.index;
            break;
        }
        default:
        {
            spdlog::error("Unsupported resource type for creation.");
            break;
        }
        }

        return handle;
    }

    RenderGraph Engine::createRenderGraph()
    {
        RenderGraph graph(this);
        return graph;
    }

    RenderTargetHandle Engine::createRenderTarget(const std::string &name, uint32_t width, uint32_t height, Format colorFormat, Format depthFormat)
    {
        auto widthToUse = (width == 0) ? static_cast<uint32_t>(config.windowWidth) : width;
        auto heightToUse = (height == 0) ? static_cast<uint32_t>(config.windowHeight) : height;
        auto colorFormatToUse = (colorFormat == Format::UNKNOWN) ? swapchain.imageFormat : colorFormat;
        auto depthFormatToUse = (depthFormat == Format::UNKNOWN) ? Format::D24_UNORM_S8_UINT : depthFormat;

        return ctx.createRenderTarget(name, widthToUse, heightToUse, colorFormatToUse, depthFormatToUse);
    }

    void Engine::beginFrame(CameraHandle camera)
    {
        // wait for the gpu to finish rendering the previous frame
        auto &currentFrame = frameResources[frameCount];
        ctx.waitForFence(currentFrame.inFlightFence);
        ctx.resetFence(currentFrame.inFlightFence);

        this->window.pollEvents(camera);

        if (this->resized)
        {
            recreateSwapchain();
            this->resized = false;
        }

        // acquire the next image from the swapchain
        ctx.acquireNextImage(currentFrame.readyToDrawSemaphore, UINT64_MAX, imageIdx);

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);

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

        ctx.endCommandBuffer(currentFrame.commandBuffer);

        ctx.submit(currentFrame.commandBuffer,
                   {currentFrame.readyToDrawSemaphore},
                   {swapchain.readyToPresentSemaphores[imageIdx]},
                   currentFrame.inFlightFence);

        ctx.present(imageIdx, swapchain.readyToPresentSemaphores[imageIdx]);

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering
    }

    // TODO: delete
    void Engine::beginOffscreenFrame(const RenderTargetHandle &renderTarget, CameraHandle camera)
    {
        auto &currentFrame = frameResources[frameCount];
        ctx.waitForFence(currentFrame.inFlightFence);
        ctx.resetFence(currentFrame.inFlightFence);

        this->window.pollEvents(camera);

        // record commands for the current frame
        ctx.beginCommandBuffer(currentFrame.commandBuffer);

        const RenderTarget &renderTargetData = ctx.getRenderTarget(renderTarget);

        // transition render target images to be ready for rendering
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
        ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);

        // begin dynamic rendering
        ctx.beginRendering(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], renderTargetData.depthAttachment[frameCount]);
        ctx.setViewport(currentFrame.commandBuffer, 0.0f, 0.0f, static_cast<float>(renderTargetData.width), static_cast<float>(renderTargetData.height));
        ctx.setScissor(currentFrame.commandBuffer, 0, 0, renderTargetData.width, renderTargetData.height);
    }

    // TODO: delete
    void Engine::endOffscreenFrame()
    {
        auto &currentFrame = frameResources[frameCount];

        ctx.endRendering(currentFrame.commandBuffer);
        ctx.endCommandBuffer(currentFrame.commandBuffer);

        ctx.submit(currentFrame.commandBuffer,
                   {},
                   {},
                   currentFrame.inFlightFence);

        frameCount = (frameCount + 1) % MAX_FRAMES_IN_FLIGHT; // Toggle between 0 and 1 for double buffering
    }

    void Engine::beginPass()
    {
        auto currentFrame = frameResources[frameCount];

        if (currentRenderTarget.isValid())
        {
            auto renderTargetData = ctx.getRenderTarget(currentRenderTarget);

            // transition render target images to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
        }
        else
        {
            // transition swapchain image to be ready for rendering
            ctx.transitionImageLayout(currentFrame.commandBuffer, swapchain.imageHandles[imageIdx], TextureUsage::UNKNOWN, TextureUsage::COLOR_ATTACHMENT);
            ctx.transitionImageLayout(currentFrame.commandBuffer, currentFrame.depthTexture, TextureUsage::UNKNOWN, TextureUsage::DEPTH_STENCIL_ATTACHMENT);
        }
    }

    void Engine::endPass()
    {
        auto currentFrame = frameResources[frameCount];

        if (currentRenderTarget.isValid())
        {
            auto renderTargetData = ctx.getRenderTarget(currentRenderTarget);

            // transition render target images to be ready for sampling
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.colorAttachment[frameCount], TextureUsage::COLOR_ATTACHMENT, TextureUsage::SAMPLED);
            ctx.transitionImageLayout(currentFrame.commandBuffer, renderTargetData.depthAttachment[frameCount], TextureUsage::DEPTH_STENCIL_ATTACHMENT, TextureUsage::SAMPLED);
        }

        // TODO: insert barrier to ensure the pass has finished before subsequent operations
    }

    void Engine::setRenderTarget(const RenderTargetHandle &renderTarget)
    {
        this->currentRenderTarget = renderTarget;
    }

    void Engine::resetRenderTarget()
    {
        this->currentRenderTarget = RenderTargetHandle{};
    }

    void Engine::setScene(SceneHandle scene)
    {
        this->currentScene = scene;
    }

    SceneHandle Engine::getScene() const
    {
        return currentScene;
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
            depthTextureDesc.type = ResourceType::TEXTURE;
            depthTextureDesc.name = "DepthTexture for Frame " + std::to_string(i);
            depthTextureDesc.texture.width = static_cast<uint32_t>(config.windowWidth);
            depthTextureDesc.texture.height = static_cast<uint32_t>(config.windowHeight);
            depthTextureDesc.texture.format = Format::D24_UNORM_S8_UINT; // TODO: check this format
            depthTextureDesc.texture.usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT;

            frameResources[i].depthTexture = ctx.createTexture(depthTextureDesc);
        }
    }

    void Engine::render(SceneHandle scene, CameraHandle camera)
    {
        beginFrame(camera);

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        auto cam = registery.cameras[camera.index];
        auto camTransform = cam.transform;
        auto camPos = camTransform.position;

        auto projection = getProjectionMatrix(cam.projection);
        auto view = glm::translate(glm::mat4(1.0f), camPos);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                             glm::mat4_cast(modelTransform.rotation) *
                             glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet, 0);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.vertexIndexBuffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                shaderDataIdx++;
            }
        }

        endFrame();
    }

    void Engine::render(SceneHandle scene, CameraHandle camera, const RenderTargetHandle &renderTarget)
    {
        beginOffscreenFrame(renderTarget, camera);

        auto compiledScene = compileScene(scene);
        auto &currentFrame = frameResources[frameCount];
        auto commandBuffer = currentFrame.commandBuffer;
        auto shaderDataBuffer = currentFrame.shaderDataBuffer;

        auto cam = registery.cameras[camera.index];
        auto camTransform = cam.transform;
        auto camPos = camTransform.position;

        auto projection = getProjectionMatrix(cam.projection);
        auto view = glm::translate(glm::mat4(1.0f), camPos);

        auto shaderDataBufferAddress = ctx.getBufferDeviceAddress(shaderDataBuffer);

        auto shaderDataIdx = 0;
        for (const auto &[materialHandle, entitySet] : compiledScene.materialToMeshes)
        {
            auto pipeline = compiledScene.materialToPipeline[materialHandle];
            ctx.bindPipeline(commandBuffer, pipeline);
            for (const auto &entityHandle : entitySet)
            {
                auto modelTransform = registery.entities[entityHandle.index].transform;

                auto model = glm::translate(glm::mat4(1.0f), modelTransform.position) *
                             glm::mat4_cast(modelTransform.rotation) *
                             glm::scale(glm::mat4(1.0f), modelTransform.scale);

                auto baseOffset = (sizeof(projection) + sizeof(view) + sizeof(model)) * shaderDataIdx;
                ctx.fillBuffer(shaderDataBuffer, &projection, sizeof(projection), baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &view, sizeof(view), sizeof(projection) + baseOffset);
                ctx.fillBuffer(shaderDataBuffer, &model, sizeof(model), sizeof(projection) + sizeof(view) + baseOffset);

                auto bufferHandle = compiledScene.meshData[entityHandle];
                auto desc = ctx.getResourceDesc(bufferHandle);
                ctx.bindDescriptorSet(commandBuffer, pipeline, bindlessDescriptorSet);
                ctx.bindVertexBuffer(commandBuffer, bufferHandle, 0);
                ctx.bindIndexBuffer(commandBuffer, bufferHandle, desc.buffer.vertexIndexBuffer.offset, Format::U32_UINT);
                auto targetShaderDataBufferAddress = shaderDataBufferAddress + baseOffset;
                ctx.pushConstants(commandBuffer, pipeline, ShaderType::VERTEX, &targetShaderDataBufferAddress, sizeof(targetShaderDataBufferAddress), 0);
                ctx.drawIndexed(commandBuffer, static_cast<uint32_t>(desc.buffer.vertexIndexBuffer.indexCount), 1, 0, 0, 0);
                shaderDataIdx++;
            }
        }

        endOffscreenFrame();
    }

    bool Engine::running() const
    {
        return isRunning;
    }

    FrameResources &Engine::getCurrentFrameResources()
    {
        return frameResources[frameCount % MAX_FRAMES_IN_FLIGHT];
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

    RenderTargetHandle Engine::getNextRenderTargetHandle()
    {
        return RenderTargetHandle{nextHandle.renderTarget++, 1};
    }

    DescriptorSetLayoutHandle Engine::getNextDescriptorSetLayoutHandle()
    {
        return DescriptorSetLayoutHandle{nextHandle.descriptorSetLayout++, 1};
    }

    DescriptorSetHandle Engine::getNextDescriptorSetHandle()
    {
        return DescriptorSetHandle{nextHandle.descriptorSet++, 1};
    }

    DescriptorPoolHandle Engine::getNextDescriptorPoolHandle()
    {
        return DescriptorPoolHandle{nextHandle.descriptorPool++, 1};
    }

    MaterialHandle Engine::getNextMaterialHandle()
    {
        return MaterialHandle{nextHandle.material++, 1};
    }

    CameraHandle Engine::getNextCameraHandle()
    {
        return CameraHandle{nextHandle.camera++, 1};
    }

    EntityHandle Engine::getNextEntityHandle()
    {
        return EntityHandle{nextHandle.entity++, 1};
    }

    LightHandle Engine::getNextLightHandle()
    {
        return LightHandle{nextHandle.light++, 1};
    }
}
