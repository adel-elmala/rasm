#include "rasm/mesh.h"
#include "rasm/engine.h"
#include "assets/meshImport.h"
#include "platform/paths.h"
#include "renderer/uberMaterialTypes.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        // A sphere around the bounding box: not the tightest, but one pass and good enough to cull with.
        void computeBounds(const std::vector<Vertex> &vertices, MeshGeometry &geometry)
        {
            if (vertices.empty())
                return;

            glm::vec3 minPos = vertices[0].pos;
            glm::vec3 maxPos = vertices[0].pos;
            for (const auto &vertex : vertices)
            {
                minPos = glm::min(minPos, vertex.pos);
                maxPos = glm::max(maxPos, vertex.pos);
            }

            geometry.boundsCenter = (minPos + maxPos) * 0.5f;
            float radius2 = 0.0f;
            for (const auto &vertex : vertices)
            {
                const glm::vec3 d = vertex.pos - geometry.boundsCenter;
                radius2 = std::max(radius2, glm::dot(d, d));
            }
            geometry.boundsRadius = glm::sqrt(radius2);
        }
    }

    MeshHandle Engine::createMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        auto handle = handleManager.getNextMeshHandle();
        if (handle.index >= MAX_MESHES)
        {
            spdlog::error("Exceeded maximum number of meshes.");
            return {};
        }

        Mesh mesh{.state = MeshState::READY, .subMeshes = {SubMesh{.transform = glm::mat4(1.0f)}}};
        if (!uploadGeometry({std::move(vertices), std::move(indices)}, mesh.subMeshes[0].geometry))
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
        auto extension = path.substr(path.find_last_of(".") + 1);
        if (supportedMeshExtensions.find(extension) == supportedMeshExtensions.end())
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

        // The worker reads and decodes the file; beginFrame uploads the result (finishLoads).
        pendingMeshes.push_back({handle, path, std::async(std::launch::async, importMeshFile, path)});

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

    // Appends to the engine-wide geometry pool and its geometry table. Nothing is ever freed from them: meshes
    // are not destroyed yet (D4). No frame has read the new row yet, so the table needs one copy, not one per slot.
    bool Engine::uploadGeometry(const ObjRaw &raw, MeshGeometry &geometry)
    {
        if (!geometryFits(raw.vertices.size(), raw.indices.size(), 1))
            return false;

        geometry = {.firstVertex = geometryVertexCount,
                    .vertexCount = static_cast<uint32_t>(raw.vertices.size()),
                    .firstIndex = geometryIndexCount,
                    .indexCount = static_cast<uint32_t>(raw.indices.size()),
                    .geometryIndex = geometryCount};
        computeBounds(raw.vertices, geometry);

        if (!raw.vertices.empty() && !ctx.fillBuffer(geometryVertexBuffer, raw.vertices.data(), raw.vertices.size() * sizeof(Vertex), static_cast<size_t>(geometry.firstVertex) * sizeof(Vertex)))
            return false;
        if (!raw.indices.empty() && !ctx.fillBuffer(geometryIndexBuffer, raw.indices.data(), raw.indices.size() * sizeof(uint32_t), static_cast<size_t>(geometry.firstIndex) * sizeof(uint32_t)))
            return false;

        const gfx::GeometryData row{.firstIndex = geometry.firstIndex,
                                    .indexCount = geometry.indexCount,
                                    .vertexOffset = static_cast<int32_t>(geometry.firstVertex),
                                    .boundsCenter = geometry.boundsCenter,
                                    .boundsRadius = geometry.boundsRadius};
        if (!ctx.fillBuffer(geometryTableBuffer, &row, sizeof(row), static_cast<size_t>(geometryCount) * sizeof(row)))
            return false;

        geometryVertexCount += geometry.vertexCount;
        geometryIndexCount += geometry.indexCount;
        ++geometryCount;
        return true;
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
        }
        if (!geometryFits(vertexCount, indexCount, raw.subGltfRaws.size()))
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
            if (!uploadGeometry(sub.objRaw, subMesh.geometry))
            {
                spdlog::error("Failed to upload mesh: {}", load.path);
                mesh.subMeshes.clear();
                return;
            }
            mesh.subMeshes.push_back(subMesh);
        }

        mesh.state = MeshState::READY;
        spdlog::info("Loaded mesh {}: {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, mesh.subMeshes.size(), geometryVertexCount, config.geometryPoolVertices, geometryIndexCount, config.geometryPoolIndices);
    }

    bool Engine::geometryFits(size_t vertexCount, size_t indexCount, size_t subMeshCount) const
    {
        if (vertexCount <= config.geometryPoolVertices - geometryVertexCount && indexCount <= config.geometryPoolIndices - geometryIndexCount &&
            subMeshCount <= config.geometryPoolSubMeshes - geometryCount)
            return true;
        spdlog::error("The geometry pool is full: {} vertices, {} indices and {} sub-meshes do not fit ({}/{} vertices, {}/{} indices and {}/{} sub-meshes in use). "
                      "EngineConfig::geometryPoolVertices, geometryPoolIndices and geometryPoolSubMeshes set its size.",
                      vertexCount, indexCount, subMeshCount, geometryVertexCount, config.geometryPoolVertices, geometryIndexCount, config.geometryPoolIndices,
                      geometryCount, config.geometryPoolSubMeshes);
        return false;
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
            meshSize.indicesByteSize += subMesh.geometry.indexCount * sizeof(uint32_t);
        }
        return meshSize;
    }
}
