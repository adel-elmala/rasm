#include "rasm/mesh.h"
#include "rasm/engine.h"
#include "assets/meshImport.h"
#include "assets/meshLod.h"
#include "platform/paths.h"
#include "renderer/geometryPool.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

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
    }

    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
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

        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::error("Exceeded maximum number of meshes.");
            return {};
        }

        ObjRaw raw{std::move(vertices), std::move(indices)};
        if (config.meshLods)
            buildMeshLods(raw);

        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!geometryPool->upload(ctx, raw, mesh.subMeshes[0].geometry))
            return {};

        registry.meshes.resize(handle.index + 1);
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
            if (registry.meshes[it->second.index].state != MeshState::FAILED)
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
            handle = handleManager.getNextMeshHandle();
            if (handle.index >= MAX_MESHES)
            {
                spdlog::warn("Exceeded maximum number of meshes.");
                return MeshHandle{};
            }
            registry.loadedMeshes[path] = handle;
            registry.meshes.resize(handle.index + 1);
        }
        registry.meshes[handle.index] = Mesh{};

        // The worker reads and decodes the file and builds the LODs; beginFrame uploads the result (finishLoads).
        const bool lods = config.meshLods;
        auto import = [path, lods]()
        {
            GLTFRaw raw = importMeshFile(path);
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
        return mesh.isValid() && mesh.index < registry.meshes.size() && registry.meshes[mesh.index].state == MeshState::READY;
    }

    void Engine::finishMeshLoad(PendingMeshLoad &load)
    {
        ZoneScoped;

        // The CPU copy of the geometry and the glTF model are freed when raw goes out of scope (P6).
        GLTFRaw raw = load.result.get();
        Mesh &mesh = registry.meshes[load.handle.index];
        mesh.state = MeshState::FAILED;
        if (!raw.loaded)
            return;

        // The whole mesh must fit before anything is created: nothing is freed from the pool, so a mesh failing
        // halfway would leak the geometry, materials and bindless slots of its first sub-meshes.
        size_t vertexCount = 0;
        size_t indexCount = 0;
        for (const auto &sub : raw.subGltfRaws)
        {
            vertexCount += sub.objRaw.vertices.size();
            indexCount += sub.objRaw.indices.size();
            for (const auto &lod : sub.objRaw.lods)
                indexCount += lod.indices.size();
        }
        if (!geometryPool->fits(vertexCount, indexCount, raw.subGltfRaws.size()))
        {
            spdlog::error("Failed to upload mesh: {}", load.path);
            return;
        }

        GltfImportCache cache;
        for (const auto &sub : raw.subGltfRaws)
        {
            SubMesh subMesh{.transform = sub.transform};
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

        mesh.state = MeshState::READY;
        spdlog::info("Loaded mesh {}: {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, mesh.subMeshes.size(), geometryPool->vertexCount(), config.geometryPoolVertices, geometryPool->indexCount(), config.geometryPoolIndices);
    }

    MeshSize Engine::getMeshByteSize(const MeshHandle &handle) const
    {
        if (!handle.isValid() || handle.index >= registry.meshes.size())
        {
            spdlog::error("Mesh handle not found for getting byte size.");
            return {};
        }

        MeshSize meshSize{};
        for (const auto &subMesh : registry.meshes[handle.index].subMeshes)
        {
            meshSize.verticesByteSize += subMesh.geometry.vertexCount * sizeof(Vertex);
            meshSize.indicesByteSize += (subMesh.geometry.indexCount + subMesh.geometry.lodIndexCount) * sizeof(uint32_t);
        }
        return meshSize;
    }
}
