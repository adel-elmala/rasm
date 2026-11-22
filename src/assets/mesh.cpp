#include "rasm/mesh.h"
#include "rasm/engine.h"
#include "assets/ktxImage.h"
#include "assets/meshImport.h"
#include "assets/meshLod.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace rasm
{
    namespace
    {
        const std::unordered_set<std::string> supportedMeshExtensions = {"obj", "gltf", "glb"};

        uint32_t toUnorm8(float value)
        {
            return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        float encodeSrgb(float linear)
        {
            linear = std::clamp(linear, 0.0f, 1.0f);
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }
    }

    // RGB is stored sRGB-encoded, so 8 bits keep the dark shades that linear values lose; the vertex shader decodes it
    // back to linear before interpolation. Alpha stays linear. R is the lowest byte.
    uint32_t packVertexColor(const glm::vec4 &linearColor)
    {
        return toUnorm8(encodeSrgb(linearColor.r)) | toUnorm8(encodeSrgb(linearColor.g)) << 8 | toUnorm8(encodeSrgb(linearColor.b)) << 16 | toUnorm8(linearColor.a) << 24;
    }

    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<uint32_t> colors)
    {
        if (!colors.empty() && colors.size() != vertices.size())
        {
            spdlog::error("createMesh: {} colors for {} vertices; give one per vertex, or none.", colors.size(), vertices.size());
            return {};
        }
        // The importers check their indices; the application's are checked here, before the LOD builder reads them.
        if (indices.size() % 3 != 0)
        {
            spdlog::error("createMesh: {} indices is not a whole number of triangles.", indices.size());
            return {};
        }
        for (uint32_t index : indices)
        {
            if (index >= vertices.size())
            {
                spdlog::error("createMesh: index {} is out of range for {} vertices.", index, vertices.size());
                return {};
            }
        }

        ObjRaw raw{.vertices = std::move(vertices), .indices = std::move(indices), .colors = std::move(colors)};
        if (config.meshLods)
            buildMeshLods(raw);

        // The handle is taken once the geometry is in the pool, so a mesh that does not fit uses no index.
        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!geometryPool->upload(ctx, raw, mesh.subMeshes[0].geometry))
            return {};

        const HandleSlot slot = registry.meshHandles.allocate();
        const MeshHandle handle{slot.index, slot.generation};
        registry.meshes.resize(registry.meshHandles.size());
        registry.meshes[handle.index] = std::move(mesh);

        return handle;
    }

    MeshHandle Engine::loadMesh(const std::string &assetPath)
    {
        const std::string path = resolveAssetPath(assetPath);

        // A path that failed to load keeps its handle, and loading it again retries on that handle.
        MeshHandle handle;
        if (auto it = registry.loadedMeshes.find(path); it != registry.loadedMeshes.end())
        {
            if (!isLive(it->second) || registry.meshes[it->second.index].state != MeshState::FAILED)
                return it->second;
            handle = it->second;
        }

        // find if the model extension is supported, if not return invalid handle
        auto extension = path.substr(path.find_last_of('.') + 1);
        if (!supportedMeshExtensions.contains(extension))
        {
            spdlog::error("Unsupported mesh format: {}", extension);
            return MeshHandle{};
        }

        // A missing file is reported here, as an invalid handle. A file that does not parse fails the mesh later.
        std::error_code error;
        if (!std::filesystem::is_regular_file(pathFromUtf8(path), error))
        {
            spdlog::error("Mesh file not found: {}", path);
            return MeshHandle{};
        }

        if (!handle.isValid())
        {
            const HandleSlot slot = registry.meshHandles.allocate();
            handle = {slot.index, slot.generation};
            registry.loadedMeshes[path] = handle;
            registry.meshes.resize(registry.meshHandles.size());
        }
        registry.meshes[handle.index] = Mesh{};

        // The worker reads and decodes the file and builds the LODs; beginFrame uploads the result (finishLoads).
        const bool lods = config.meshLods;
        const uint64_t textureFormats = getSupportedTextureFormats(ctx);
        auto import = [path, lods, textureFormats]()
        {
            GLTFRaw raw = importMeshFile(path, textureFormats);
            if (lods)
            {
                for (auto &sub : raw.subGltfRaws)
                    buildMeshLods(sub.objRaw);
            }
            return raw;
        };
        pendingMeshes.push_back({handle, path, std::async(std::launch::async, import)});

        return handle;
    }

    MeshHandle Engine::loadGLTF(const std::string &path)
    {
        return loadMesh(path);
    }

    bool Engine::isLoaded(MeshHandle mesh) const
    {
        return isLive(mesh) && registry.meshes[mesh.index].state == MeshState::READY;
    }

    void Engine::finishMeshLoad(PendingMeshLoad &load)
    {
        ZoneScoped;

        // The CPU copy of the geometry and the glTF model are freed when raw goes out of scope (P6).
        const auto mainThreadStart = std::chrono::steady_clock::now();
        GLTFRaw raw = load.result.get();
        Mesh &mesh = registry.meshes[load.handle.index];
        mesh.state = MeshState::FAILED;
        if (!raw.loaded)
            return;

        // The whole mesh must fit before anything is created: nothing is freed from the pool, so a mesh failing
        // halfway would leak the geometry, materials and bindless slots of its first sub-meshes.
        size_t vertexCount = 0;
        size_t indexCount = 0;
        size_t colorCount = 0;
        size_t skinCount = 0;
        size_t morphCount = 0;
        for (const auto &sub : raw.subGltfRaws)
        {
            vertexCount += sub.objRaw.vertices.size();
            indexCount += sub.objRaw.indices.size();
            colorCount += sub.objRaw.colors.size();
            skinCount += sub.objRaw.skin.size();
            morphCount += sub.objRaw.morphs.size();
            for (const auto &lod : sub.objRaw.lods)
                indexCount += lod.indices.size();
        }
        if (!geometryPool->fits(vertexCount, indexCount, colorCount, skinCount, morphCount, raw.subGltfRaws.size()))
        {
            spdlog::error("Failed to upload mesh: {}", load.path);
            return;
        }
        const uint64_t animationBytes = getMeshAnimationBytes(raw);
        if (animationBytes > config.animationPoolBytes - getAnimationPoolUsed())
        {
            spdlog::error("The animation pool is full: mesh {} needs {} B ({}/{} B in use). EngineConfig::animationPoolBytes sets its size.",
                          load.path, animationBytes, getAnimationPoolUsed(), config.animationPoolBytes);
            return;
        }

        GltfImportCache cache{.raw = &raw};
        for (const auto &sub : raw.subGltfRaws)
        {
            SubMesh subMesh{.transform = sub.transform, .animatedNode = sub.animatedNode};
            if (raw.model)
                subMesh.material = loadMaterial(*raw.model, sub.materialIndex, cache);
            if (!geometryPool->upload(ctx, sub.objRaw, subMesh.geometry))
            {
                spdlog::error("Failed to upload mesh: {}", load.path);
                mesh.subMeshes.clear();
                return;
            }
            mesh.subMeshes.push_back(subMesh);
        }

        if (!uploadMeshAnimation(load.handle, raw))
        {
            spdlog::error("Failed to upload the animations of mesh {}", load.path);
            mesh.subMeshes.clear();
            return;
        }
        for (const AnimationClipRaw &clip : raw.animation.clips)
            mesh.animations.push_back(clip.info);
        mesh.morphTargetNames = raw.animation.morphTargetNames;

        mesh.state = MeshState::READY;
        spdlog::info("Loaded mesh {} ({:.1f} ms on the main thread): {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mainThreadStart).count(), mesh.subMeshes.size(), geometryPool->vertexCount(), config.geometryPoolVertices, geometryPool->indexCount(), config.geometryPoolIndices);
    }

    MeshSize Engine::getMeshByteSize(const MeshHandle &handle) const
    {
        if (!isLive(handle))
        {
            spdlog::error("Mesh handle not found for getting byte size.");
            return {};
        }

        MeshSize meshSize{};
        for (const auto &subMesh : registry.meshes[handle.index].subMeshes)
        {
            meshSize.verticesByteSize += subMesh.geometry.vertexCount * sizeof(Vertex) + subMesh.geometry.colorCount * sizeof(uint32_t) + subMesh.geometry.skinCount * sizeof(SkinVertex) +
                                         subMesh.geometry.morphCount * sizeof(MorphDelta);
            meshSize.indicesByteSize += (subMesh.geometry.indexCount + subMesh.geometry.lodIndexCount) * sizeof(uint32_t);
        }
        return meshSize;
    }
}
