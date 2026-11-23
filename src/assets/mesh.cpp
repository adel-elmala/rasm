#include "rasm/mesh.h"
#include "rasm/engine.h"
#include "assets/ktxImage.h"
#include "assets/loadJobs.h"
#include "assets/meshImport.h"
#include "assets/meshLod.h"
#include "core/workerPool.h"
#include "platform/paths.h"
#include "renderer/animation.h"
#include "renderer/geometryPool.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
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

        // What a load's result holds in memory until finishMeshLoad: geometry, LODs and texture levels.
        uint64_t getRawBytes(const GLTFRaw &raw)
        {
            uint64_t bytes = 0;
            for (const SubGLTFRaw &sub : raw.subGltfRaws)
            {
                const ObjRaw &obj = sub.objRaw;
                bytes += obj.vertices.size() * sizeof(Vertex) + obj.indices.size() * sizeof(uint32_t) + obj.colors.size() * sizeof(uint32_t) +
                         obj.skin.size() * sizeof(SkinVertex) + obj.morphs.size() * sizeof(MorphDelta);
                for (const MeshLod &lod : obj.lods)
                    bytes += lod.indices.size() * sizeof(uint32_t);
            }
            for (const auto &[key, levels] : raw.ktxImages)
                bytes += levels->levels.size();
            for (const auto &[key, levels] : raw.imageLevels)
                bytes += levels->levels.size();
            return bytes;
        }

        // Runs on the job that ends last. A dropped load skips the work but still ends, so its bytes go back.
        void completeMeshLoad(MeshLoadJob &job, WorkerPool *pool)
        {
            if (!job.ticket.cancelled)
                finishImageWork(job.raw);
            job.images.clear();
            pool->setLoadBytes(job.ticket, getRawBytes(job.raw));
            pool->finishLoad(job.ticket);
        }

        void endMeshChildJob(const std::shared_ptr<MeshLoadJob> &job, WorkerPool *pool)
        {
            if (job->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                completeMeshLoad(*job, pool);
        }

        // Each image and each sub-mesh's LODs become a child job. None is waited for: the last one to end finishes the
        // load, so no worker is ever held.
        void spawnMeshChildJobs(const std::shared_ptr<MeshLoadJob> &job, WorkerPool *pool, uint32_t lodCount)
        {
            const auto imageCount = static_cast<uint32_t>(job->images.size());
            if (imageCount + lodCount == 0 || job->ticket.cancelled)
            {
                completeMeshLoad(*job, pool);
                return;
            }
            job->remaining = imageCount + lodCount;
            for (uint32_t i = 0; i < imageCount; ++i)
                pool->pushChild([job, pool, i]()
                                {
                                    if (!job->ticket.cancelled)
                                        runImageWork(*job->raw.model, job->images[i]);
                                    endMeshChildJob(job, pool); });
            for (uint32_t i = 0; i < lodCount; ++i)
                pool->pushChild([job, pool, i]()
                                {
                                    if (!job->ticket.cancelled)
                                        buildMeshLods(job->raw.subGltfRaws[i].objRaw);
                                    endMeshChildJob(job, pool); });
        }

        // The import job reads the file and plans the images. Until it is parsed, the file's size stands for the load
        // (a .gltf's buffers and images are counted once read). Then it holds its geometry and its images' file bytes,
        // and the decoded images are a next step that waits for the budget like any load.
        void runMeshImport(const std::shared_ptr<MeshLoadJob> &job, WorkerPool *pool, const std::string &path, bool lods, uint64_t textureFormats)
        {
            ZoneScoped;

            if (job->ticket.cancelled)
            {
                pool->finishLoad(job->ticket);
                return;
            }
            job->raw = parseMeshFile(path, textureFormats, job->images);
            uint64_t imageBytes = 0;
            uint64_t fileBytes = 0;
            for (const GltfImageWork &work : job->images)
            {
                imageBytes += work.bytes;
                fileBytes += work.fileBytes;
            }
            pool->setLoadBytes(job->ticket, getRawBytes(job->raw) + fileBytes);

            const auto lodCount = lods ? static_cast<uint32_t>(job->raw.subGltfRaws.size()) : 0u;
            if (imageBytes == 0 || job->ticket.cancelled)
            {
                spawnMeshChildJobs(job, pool, lodCount);
                return;
            }
            pool->pushNext({.run = [job, pool, lodCount]()
                            { spawnMeshChildJobs(job, pool, lodCount); },
                            .bytes = imageBytes,
                            .ticket = std::shared_ptr<LoadTicket>(job, &job->ticket)});
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
        if (!geometryPool->check(raw))
            return {};
        if (!geometryPool->allocate(raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->logFull(1);
            return {};
        }
        if (!geometryPool->write(ctx, raw, mesh.subMeshes[0].geometry))
        {
            geometryPool->free(mesh.subMeshes[0].geometry);
            return {};
        }

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
            if (!isLive(it->second))
                registry.loadedMeshes.erase(it);
            else if (registry.meshes[it->second.index].state != MeshState::FAILED)
                return it->second;
            else
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
            registry.meshPaths.resize(registry.meshHandles.size());
            registry.meshPaths[handle.index] = path;
        }
        // A retry keeps the count of the entities that already use the mesh.
        const uint32_t users = registry.meshes[handle.index].users;
        registry.meshes[handle.index] = Mesh{.users = users};

        // The workers read and decode the file and build the LODs; beginFrame uploads the result (finishLoads). Until the
        // file is parsed, its size stands for what it will hold.
        const bool lods = config.meshLods;
        const uint64_t textureFormats = getSupportedTextureFormats(ctx);
        auto job = std::make_shared<MeshLoadJob>();
        WorkerPool *pool = workers.get();
        const uint64_t fileBytes = std::filesystem::file_size(pathFromUtf8(path), error);
        pool->push({.run = [job, pool, path, lods, textureFormats]()
                    { runMeshImport(job, pool, path, lods, textureFormats); },
                    .bytes = error ? 0 : fileBytes,
                    .ticket = std::shared_ptr<LoadTicket>(job, &job->ticket)});
        pendingMeshes.push_back({handle, path, std::move(job)});

        return handle;
    }

    MeshHandle Engine::loadGLTF(const std::string &path)
    {
        return loadMesh(path);
    }

    // A glTF's textures are filled after its mesh is ready, a few per frame (P2), so it counts as loaded once they are.
    bool Engine::isLoaded(MeshHandle mesh) const
    {
        if (!isLive(mesh) || registry.meshes[mesh.index].state != MeshState::READY)
            return false;
        for (const TextureHandle &texture : registry.meshes[mesh.index].ownedTextures)
        {
            if (pendingTextures.contains(texture))
                return false;
        }
        return true;
    }

    void Engine::finishMeshLoad(PendingMeshLoad &load)
    {
        ZoneScoped;

        // The CPU copy of the geometry and the glTF model are freed with the job, on a worker (P6, P26).
        const auto mainThreadStart = std::chrono::steady_clock::now();
        GLTFRaw &raw = load.job->raw;
        // Destroyed while it loaded: the result is dropped.
        if (!isLive(load.handle))
            return;
        registry.meshes[load.handle.index].state = MeshState::FAILED;
        if (!raw.loaded)
            return;

        // Every sub-mesh's ranges are taken before anything is created, so a mesh that does not fit takes nothing.
        const size_t subMeshCount = raw.subGltfRaws.size();
        std::vector<MeshGeometry> geometries(subMeshCount);
        for (const SubGLTFRaw &sub : raw.subGltfRaws)
        {
            if (!geometryPool->check(sub.objRaw))
            {
                spdlog::error("Failed to upload mesh: {}", load.path);
                return;
            }
        }
        for (size_t i = 0; i < subMeshCount; ++i)
        {
            if (!geometryPool->allocate(raw.subGltfRaws[i].objRaw, geometries[i]))
            {
                for (size_t j = 0; j < i; ++j)
                    geometryPool->free(geometries[j]);
                geometryPool->logFull(subMeshCount);
                spdlog::error("Failed to upload mesh: {}", load.path);
                return;
            }
        }
        const uint64_t animationBytes = getMeshAnimationBytes(raw);
        if (animationBytes > gpuAnimation->poolRanges.largestFree())
        {
            for (const MeshGeometry &geometry : geometries)
                geometryPool->free(geometry);
            spdlog::error("The animation pool is full: mesh {} needs {} B ({}/{} B in use, the largest free range {} B). EngineConfig::animationPoolBytes sets its size.",
                          load.path, animationBytes, getAnimationPoolUsed(), config.animationPoolBytes, gpuAnimation->poolRanges.largestFree());
            return;
        }

        // A failure past here gives back what was made: no frame has used any of it yet.
        GltfImportCache cache{.raw = &raw};
        auto fail = [&](const char *what)
        {
            load.movedBytes = cache.movedBytes;
            spdlog::error("Failed to upload the {} of mesh {}", what, load.path);
            for (const MeshGeometry &geometry : geometries)
                geometryPool->free(geometry);
            for (const auto &[index, material] : cache.materials)
            {
                if (isLive(material))
                    destroyMaterial(material);
            }
            for (const auto &[key, texture] : cache.imageTextures)
            {
                if (ctx.isAlive(texture))
                    destroyTexture(texture);
            }
            registry.meshes[load.handle.index].subMeshes.clear();
        };
        for (size_t i = 0; i < subMeshCount; ++i)
        {
            const SubGLTFRaw &sub = raw.subGltfRaws[i];
            SubMesh subMesh{.geometry = geometries[i], .transform = sub.transform, .animatedNode = sub.animatedNode};
            if (raw.model)
                subMesh.material = loadMaterial(*raw.model, sub.materialIndex, cache);
            registry.meshes[load.handle.index].subMeshes.push_back(subMesh);
            if (!geometryPool->write(ctx, sub.objRaw, subMesh.geometry))
            {
                fail("geometry");
                return;
            }
        }
        if (!uploadMeshAnimation(load.handle, raw))
        {
            fail("animations");
            return;
        }

        Mesh &mesh = registry.meshes[load.handle.index];
        for (const AnimationClipRaw &clip : raw.animation.clips)
            mesh.animations.push_back(clip.info);
        mesh.morphTargetNames = raw.animation.morphTargetNames;
        // What the import made belongs to the mesh, and goes with it (destroyMesh).
        registry.materialOwners.resize(registry.materials.size());
        for (const auto &[index, material] : cache.materials)
        {
            if (!material.isValid())
                continue;
            mesh.ownedMaterials.push_back(material);
            registry.materialOwners[material.index] = load.handle;
        }
        for (const auto &[key, texture] : cache.imageTextures)
        {
            if (texture.isValid())
                mesh.ownedTextures.push_back(texture);
        }

        // READY once the GPU has its geometry (publishUploadedLoads).
        load.movedBytes = cache.movedBytes;
        mesh.state = MeshState::LOADING;
        const GeometryPoolStats pool = geometryPool->stats();
        spdlog::info("Loaded mesh {} ({:.1f} ms on the main thread): {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mainThreadStart).count(), mesh.subMeshes.size(), pool.vertices,
                     config.geometryPoolVertices, pool.indices, config.geometryPoolIndices);
    }

    // Entities that use the mesh keep living and draw nothing. Its geometry ranges, table rows and animation pool range
    // wait for the frames in flight, which may still draw it; its materials and textures go with it.
    void Engine::destroyMesh(MeshHandle mesh)
    {
        if (!isLive(mesh))
        {
            spdlog::error("destroyMesh: mesh handle {{{}, {}}} is invalid or was destroyed.", mesh.index, mesh.generation);
            return;
        }
        // A load still in the pool is dropped: a queued job never runs, a running one gives its result up.
        for (auto it = pendingMeshes.begin(); it != pendingMeshes.end(); ++it)
        {
            if (it->handle == mesh)
            {
                LoadTicket &ticket = it->job->ticket;
                dropPendingLoad(ticket, std::move(it->job));
                pendingMeshes.erase(it);
                break;
            }
        }
        Mesh &meshRef = registry.meshes[mesh.index];
        // The usual order destroys the entities first, and then nothing is searched.
        for (size_t i = 1; meshRef.users > 0 && i < registry.entities.size(); ++i)
        {
            Entity &entity = registry.entities[i];
            if (!(entity.mesh == mesh) || !isLive(entity.handle))
                continue;
            releaseEntityRows(entity.handle);
            entity.mesh = {};
            --meshRef.users;
        }

        std::vector<MeshGeometry> geometries;
        for (const SubMesh &subMesh : meshRef.subMeshes)
        {
            if (subMesh.geometry.allocated)
                geometries.push_back(subMesh.geometry);
        }
        if (!geometries.empty())
            ctx.releaseAfterGpu([this, geometries]()
                                {
                                    for (const MeshGeometry &geometry : geometries)
                                        geometryPool->free(geometry); });
        releaseMeshAnimation(mesh);

        const std::vector<MaterialHandle> materials = std::move(meshRef.ownedMaterials);
        const std::vector<TextureHandle> textures = std::move(meshRef.ownedTextures);
        if (mesh.index < registry.meshPaths.size() && !registry.meshPaths[mesh.index].empty())
        {
            auto it = registry.loadedMeshes.find(registry.meshPaths[mesh.index]);
            if (it != registry.loadedMeshes.end() && it->second == mesh)
                registry.loadedMeshes.erase(it);
            registry.meshPaths[mesh.index].clear();
        }
        registry.meshes[mesh.index] = Mesh{};
        registry.meshHandles.release(mesh.index, mesh.generation);

        for (MaterialHandle material : materials)
        {
            if (isLive(material))
                destroyMaterial(material);
        }
        for (TextureHandle texture : textures)
        {
            if (ctx.isAlive(texture))
                destroyTexture(texture);
        }
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
