#include "assets/assetLoaderImpl.h"
#include "assets/ktxImage.h"
#include "assets/loadJobs.h"
#include "assets/meshImport.h"
#include "assets/meshLod.h"
#include "assets/meshlets.h"
#include "core/workerPool.h"
#include "platform/paths.h"
#include "renderer/animation.h"
#include "renderer/geometryPool.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
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

        // What a load's result holds in memory until finishMeshLoad: geometry, LODs and texture levels.
        uint64_t getRawBytes(const GLTFRaw &raw)
        {
            uint64_t bytes = 0;
            for (const SubGLTFRaw &sub : raw.subGltfRaws)
            {
                const ObjRaw &obj = sub.objRaw;
                bytes += obj.vertices.size() * sizeof(Vertex) + obj.indices.size() * sizeof(uint32_t) + obj.colors.size() * sizeof(uint32_t) +
                         obj.skin.size() * sizeof(SkinVertex) + obj.morphs.size() * sizeof(MorphDelta) + obj.meshlets.size() * sizeof(Meshlet) + obj.meshletData.size() * sizeof(uint32_t);
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

        // Each image and each sub-mesh's LODs and meshlets become a child job. None is waited for: the last one to end
        // finishes the load, so no worker is ever held.
        void spawnMeshChildJobs(const std::shared_ptr<MeshLoadJob> &job, WorkerPool *pool, uint32_t subMeshJobs, bool lods)
        {
            const auto imageCount = static_cast<uint32_t>(job->images.size());
            if (imageCount + subMeshJobs == 0 || job->ticket.cancelled)
            {
                completeMeshLoad(*job, pool);
                return;
            }
            job->remaining = imageCount + subMeshJobs;
            for (uint32_t i = 0; i < imageCount; ++i)
                pool->pushChild([job, pool, i]()
                                {
                                    if (!job->ticket.cancelled)
                                        runImageWork(*job->raw.model, job->images[i]);
                                    endMeshChildJob(job, pool); });
            for (uint32_t i = 0; i < subMeshJobs; ++i)
                pool->pushChild([job, pool, i, lods]()
                                {
                                    if (!job->ticket.cancelled && lods)
                                        buildMeshLods(job->raw.subGltfRaws[i].objRaw);
                                    if (!job->ticket.cancelled)
                                        buildMeshlets(job->raw.subGltfRaws[i].objRaw);
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

            const auto subMeshJobs = static_cast<uint32_t>(job->raw.subGltfRaws.size());
            if (imageBytes == 0 || job->ticket.cancelled)
            {
                spawnMeshChildJobs(job, pool, subMeshJobs, lods);
                return;
            }
            pool->pushNext({.run = [job, pool, subMeshJobs, lods]()
                            { spawnMeshChildJobs(job, pool, subMeshJobs, lods); },
                            .bytes = imageBytes,
                            .ticket = std::shared_ptr<LoadTicket>(job, &job->ticket)});
        }
    }

    MeshHandle AssetLoader::Impl::loadMesh(const std::string &assetPath)
    {
        const std::string path = resolveAssetPath(assetPath);

        // A path that failed to load keeps its handle, and loading it again retries on that handle.
        MeshHandle handle;
        if (auto it = loadedMeshes.find(path); it != loadedMeshes.end())
        {
            if (!renderer->isLive(it->second))
                loadedMeshes.erase(it);
            else if (renderer->registry.meshes[it->second.index].state != MeshState::FAILED)
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
            const HandleSlot slot = renderer->registry.meshHandles.allocate();
            handle = {slot.index, slot.generation};
            loadedMeshes[path] = handle;
            renderer->registry.meshes.resize(renderer->registry.meshHandles.size());
            meshPaths.resize(renderer->registry.meshHandles.size());
            meshPaths[handle.index] = path;
        }
        // A retry keeps the count of the entities that already use the mesh.
        const uint32_t users = renderer->registry.meshes[handle.index].users;
        renderer->registry.meshes[handle.index] = Mesh{.users = users};

        // The workers read and decode the file and build the LODs and meshlets; the renderer's beginFrame uploads the result
        // (finishLoads). Until the file is parsed, its size stands for what it will hold.
        const bool lods = renderer->config.meshLods;
        const uint64_t textureFormats = getSupportedTextureFormats(renderer->ctx);
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

    void AssetLoader::Impl::finishMeshLoad(PendingMeshLoad &load)
    {
        ZoneScoped;

        // The CPU copy of the geometry and the glTF model are freed with the job, on a worker (P6, P26).
        const auto mainThreadStart = std::chrono::steady_clock::now();
        GLTFRaw &raw = load.job->raw;
        // Destroyed while it loaded: the result is dropped.
        if (!renderer->isLive(load.handle))
            return;
        renderer->registry.meshes[load.handle.index].state = MeshState::FAILED;
        if (!raw.loaded)
            return;

        // Every sub-mesh's ranges are taken before anything is created, so a mesh that does not fit takes nothing.
        const size_t subMeshCount = raw.subGltfRaws.size();
        std::vector<MeshGeometry> geometries(subMeshCount);
        for (const SubGLTFRaw &sub : raw.subGltfRaws)
        {
            if (!renderer->geometryPool->check(sub.objRaw))
            {
                spdlog::error("Failed to upload mesh: {}", load.path);
                return;
            }
        }
        for (size_t i = 0; i < subMeshCount; ++i)
        {
            if (!renderer->geometryPool->allocate(raw.subGltfRaws[i].objRaw, geometries[i]))
            {
                for (size_t j = 0; j < i; ++j)
                    renderer->geometryPool->free(geometries[j]);
                renderer->geometryPool->logFull(subMeshCount);
                spdlog::error("Failed to upload mesh: {}", load.path);
                return;
            }
        }
        const uint64_t animationBytes = renderer->getMeshAnimationBytes(raw);
        if (animationBytes > renderer->gpuAnimation->poolRanges.largestFree())
        {
            for (const MeshGeometry &geometry : geometries)
                renderer->geometryPool->free(geometry);
            spdlog::error("The animation pool is full: mesh {} needs {} B ({}/{} B in use, the largest free range {} B). RendererConfig::animationPoolBytes sets its size.",
                          load.path, animationBytes, renderer->getAnimationPoolUsed(), renderer->config.animationPoolBytes, renderer->gpuAnimation->poolRanges.largestFree());
            return;
        }

        // A failure past here gives back what was made: no frame has used any of it yet.
        GltfImportCache cache{.raw = &raw};
        auto fail = [&](const char *what)
        {
            load.movedBytes = cache.movedBytes;
            spdlog::error("Failed to upload the {} of mesh {}", what, load.path);
            for (const MeshGeometry &geometry : geometries)
                renderer->geometryPool->free(geometry);
            for (const auto &[index, material] : cache.materials)
            {
                if (renderer->isLive(material))
                    renderer->destroyMaterial(material);
            }
            for (const auto &[key, texture] : cache.imageTextures)
            {
                if (renderer->ctx.isAlive(texture))
                    renderer->destroyTexture(texture);
            }
            renderer->registry.meshes[load.handle.index].subMeshes.clear();
        };
        for (size_t i = 0; i < subMeshCount; ++i)
        {
            const SubGLTFRaw &sub = raw.subGltfRaws[i];
            SubMesh subMesh{.geometry = geometries[i], .transform = sub.transform, .animatedNode = sub.animatedNode};
            if (raw.model)
                subMesh.material = loadMaterial(*raw.model, sub.materialIndex, cache);
            renderer->registry.meshes[load.handle.index].subMeshes.push_back(subMesh);
            if (!renderer->geometryPool->write(renderer->ctx, sub.objRaw, subMesh.geometry))
            {
                fail("geometry");
                return;
            }
        }
        if (!renderer->uploadMeshAnimation(load.handle, raw))
        {
            fail("animations");
            return;
        }

        Mesh &mesh = renderer->registry.meshes[load.handle.index];
        for (const AnimationClipRaw &clip : raw.animation.clips)
            mesh.animations.push_back(clip.info);
        mesh.morphTargetNames = raw.animation.morphTargetNames;
        // What the import made belongs to the mesh, and goes with it (destroyMesh).
        renderer->registry.materialOwners.resize(renderer->registry.materials.size());
        for (const auto &[index, material] : cache.materials)
        {
            if (!material.isValid())
                continue;
            mesh.ownedMaterials.push_back(material);
            renderer->registry.materialOwners[material.index] = load.handle;
        }
        for (const auto &[key, texture] : cache.imageTextures)
        {
            if (texture.isValid())
                mesh.ownedTextures.push_back(texture);
        }

        // READY once the GPU has its geometry (publishUploadedLoads).
        load.movedBytes = cache.movedBytes;
        mesh.state = MeshState::LOADING;
        const GeometryPoolStats pool = renderer->geometryPool->stats();
        spdlog::info("Loaded mesh {} ({:.1f} ms on the main thread): {} sub-meshes, geometry pool at {}/{} vertices and {}/{} indices.",
                     load.path, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mainThreadStart).count(), mesh.subMeshes.size(), pool.vertices,
                     renderer->config.geometryPoolVertices, pool.indices, renderer->config.geometryPoolIndices);
    }
}
