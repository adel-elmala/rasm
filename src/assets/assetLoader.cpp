#include "rasm/assetLoader.h"
#include "assets/assetLoaderImpl.h"
#include "platform/paths.h"
#include "renderer/environmentBake.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace rasm
{
    namespace
    {
        // Main-thread time a frame spends finishing loads. The load that crosses it still finishes.
        constexpr std::chrono::milliseconds LOAD_BUDGET(4);
        // The bytes of a texture filled between two checks of LOAD_BUDGET: about 0.6 ms of copying into the staging ring.
        constexpr uint64_t TEXTURE_FILL_PART = 4ull << 20;
    }

    // The workers start before anything else can fail, so a load on a renderer that did not start still has them. One
    // hardware thread is left for the main thread. A refused loader (the renderer has one) starts none.
    AssetLoader::Impl::Impl(Renderer &owner, const AssetLoaderConfig &loaderConfig) : renderer(&owner), config(loaderConfig)
    {
        if (!config.assetRoot.empty())
            config.assetRoot = resolveFromBaseDir(config.assetRoot);
        if (renderer->loads != nullptr)
        {
            spdlog::error("AssetLoader: the renderer has an AssetLoader already; this one refuses every call.");
            renderer = nullptr;
            return;
        }
        const uint32_t hardwareThreads = std::thread::hardware_concurrency();
        const uint32_t loadThreads = config.loadThreads > 0 ? config.loadThreads : std::max(2u, hardwareThreads) - 1;
        workers = std::make_unique<WorkerPool>(loadThreads, config.loadMemoryBudget, "RASM loader");
        renderer->loads = this;
    }

    // The renderer lives on, so what this loader leaves unfinished must not stay loading for good: what is on its way to
    // the GPU is waited for and published, then the meshes and environments still loading fail and the textures still
    // loading are destroyed (their materials lose them), as when a decode fails.
    AssetLoader::Impl::~Impl()
    {
        if (renderer == nullptr || renderer->loads != this)
        {
            dropAll();
            return;
        }
        if (renderer->running() && (renderer->ctx.isLoadUploadDone(loadUploadValue) || renderer->ctx.waitForLoadUploads(loadUploadValue)))
            publishUploadedLoads();
        std::vector<MeshHandle> meshes;
        std::vector<EnvironmentHandle> environments;
        std::vector<TextureHandle> textures;
        for (const PendingMeshLoad &load : pendingMeshes)
            meshes.push_back(load.handle);
        for (const auto &[mesh, value] : uploadingMeshes)
            meshes.push_back(mesh);
        for (const PendingEnvironmentLoad &load : pendingEnvironments)
            environments.push_back(load.handle);
        for (const auto &[texture, load] : pendingTextures)
            textures.push_back(texture);
        dropAll();
        renderer->loads = nullptr;
        for (MeshHandle mesh : meshes)
        {
            if (renderer->isLive(mesh) && renderer->registry.meshes[mesh.index].state == MeshState::LOADING)
                renderer->registry.meshes[mesh.index].state = MeshState::FAILED;
        }
        for (EnvironmentHandle environment : environments)
        {
            if (renderer->isLive(environment) && renderer->environments->environments[environment.index].state == BakeState::LOADING)
                renderer->environments->environments[environment.index].state = BakeState::FAILED;
        }
        for (TextureHandle texture : textures)
        {
            if (renderer->ctx.isAlive(texture))
                renderer->destroyTexture(texture);
        }
    }

    // Loads that still run stop at their next step, queued ones are dropped, and the workers end. Jobs never touch the
    // renderer: they write their job structs, which they hold.
    void AssetLoader::Impl::dropAll()
    {
        if (!workers)
            return;
        for (PendingMeshLoad &load : pendingMeshes)
            workers->releaseBytes(workers->dropLoad(load.job->ticket));
        for (auto &[handle, load] : pendingTextures)
        {
            if (load.job)
                workers->releaseBytes(workers->dropLoad(load.job->ticket));
        }
        for (PendingEnvironmentLoad &load : pendingEnvironments)
            workers->releaseBytes(workers->dropLoad(load.job->ticket));
        pendingMeshes.clear();
        pendingTextures.clear();
        pendingEnvironments.clear();
        uploadingMeshes.clear();
        workers->shutdown();
    }

    // The renderer goes first, against the rule: what is in flight is dropped, and nothing reaches the renderer again.
    void AssetLoader::Impl::detach()
    {
        dropAll();
        renderer = nullptr;
    }

    bool AssetLoader::Impl::attached(const char *caller) const
    {
        if (renderer == nullptr)
            spdlog::error("{}: the AssetLoader has no renderer (it was destroyed first, or had a loader already).", caller);
        return renderer != nullptr;
    }

    std::string AssetLoader::Impl::resolveAssetPath(const std::string &path) const
    {
        // Every loader expects UTF-8, whichever encoding the path arrived in.
        if (config.assetRoot.empty())
            return pathToUtf8(pathFromUtf8(path));
        return pathToUtf8((pathFromUtf8(config.assetRoot) / pathFromUtf8(path)).lexically_normal());
    }

    // A load still in the pool is dropped: a queued job never runs, a running one gives its result up. The path leaves
    // the cache, so loading it again makes a new mesh.
    void AssetLoader::Impl::forget(MeshHandle mesh)
    {
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
        if (mesh.index < meshPaths.size() && !meshPaths[mesh.index].empty())
        {
            auto it = loadedMeshes.find(meshPaths[mesh.index]);
            if (it != loadedMeshes.end() && it->second == mesh)
                loadedMeshes.erase(it);
            meshPaths[mesh.index].clear();
        }
    }

    // The load's bytes go back to the budget once a worker freed its data, from here or once it is filled (finishLoads).
    void AssetLoader::Impl::forget(TextureHandle texture)
    {
        if (auto it = pendingTextures.find(texture); it != pendingTextures.end())
        {
            if (it->second.job)
            {
                LoadTicket &ticket = it->second.job->ticket;
                dropPendingLoad(ticket, std::move(it->second.job));
            }
            else
                freeOnWorker(std::move(it->second.levels), it->second.bytes);
            pendingTextures.erase(it);
        }
        // The key may name a newer texture of the same file, when this slot's earlier one was destroyed elsewhere.
        if (texture.index < textureCacheKeys.size() && !textureCacheKeys[texture.index].empty())
        {
            auto it = loadedTextures.find(textureCacheKeys[texture.index]);
            if (it != loadedTextures.end() && it->second == texture)
                loadedTextures.erase(it);
            textureCacheKeys[texture.index].clear();
        }
    }

    void AssetLoader::Impl::forget(EnvironmentHandle environment)
    {
        for (auto it = pendingEnvironments.begin(); it != pendingEnvironments.end(); ++it)
        {
            if (it->handle == environment)
            {
                LoadTicket &ticket = it->job->ticket;
                dropPendingLoad(ticket, std::move(it->job));
                pendingEnvironments.erase(it);
                break;
            }
        }
    }

    // A renderer that did not start has no device to fill: its loads stay in the pool.
    void AssetLoader::Impl::finishLoads()
    {
        if (renderer->running())
            finishLoads(false);
    }

    void AssetLoader::Impl::waitForLoads()
    {
        if (attached("waitForLoads") && renderer->running())
            finishLoads(true);
    }

    LoadStats AssetLoader::Impl::getLoadStats() const
    {
        if (!workers)
            return {};
        const WorkerPoolStats pool = workers->stats();
        return {.workers = pool.workers, .queued = pool.queued, .running = pool.running, .decodedBytes = pool.bytes, .peakDecodedBytes = pool.peakBytes};
    }

    // Runs at the start of a frame, before anything binds the bindless set or the geometry pool, so
    // what it writes is in place for this frame's draws and its uploads go out with this frame's submit.
    // Without wait it stops at LOAD_BUDGET, so loads that finish together spread over several frames. With wait it
    // finishes what is ready, which gives bytes back to the load budget, then waits for the next load to end: a load
    // queued behind the budget can only start once the main thread has taken the ones before it.
    void AssetLoader::Impl::finishLoads(bool wait)
    {
        if (!wait)
        {
            finishReadyLoads(true);
            return;
        }
        // What is filled is loaded once its copies are done, so the wait for them comes before the one for the next load.
        for (;;)
        {
            const uint64_t seen = workers->completions();
            if (!finishReadyLoads(false))
                return;
            if (renderer->ctx.isLoadUploadDone(loadUploadValue))
                workers->waitForCompletion(seen);
            else if (!renderer->ctx.waitForLoadUploads(loadUploadValue))
            {
                spdlog::error("waitForLoads: the load uploads did not go out; some loads stay unfinished.");
                return;
            }
        }
    }

    // A finished load's decoded data is freed on a worker, and its bytes go back to the load budget once it is freed, so
    // the budget does not count a result as gone while it is still in memory (except a glTF image that two textures
    // share, P26). Freeing a few MiB gives their pages back to the OS, which took 0.15 to 0.3 ms each on the main
    // thread (P26). A free that finds the pool stopping happens here, at once.
    void AssetLoader::Impl::freeOnWorker(std::shared_ptr<const void> result, uint64_t bytes)
    {
        if (!result)
        {
            workers->releaseBytes(bytes);
            return;
        }
        WorkerPool *pool = workers.get();
        pool->pushChild([pool, result = std::move(result), bytes]() mutable
                        {
                            result.reset();
                            pool->releaseBytes(bytes); });
    }

    // A queued job never runs and a running one frees its result when it ends; a finished one's result goes to a worker
    // like a filled one's, its bytes after it (dropLoad returns them, also for a load that finished just now).
    void AssetLoader::Impl::dropPendingLoad(LoadTicket &ticket, std::shared_ptr<const void> result)
    {
        freeOnWorker(std::move(result), workers->dropLoad(ticket));
    }

    // Returns whether loads are still pending. Textures come last: a finished glTF adds its textures, each filled on its
    // own (P2), so a big model's copies spread over frames too, and waitForLoads fills them in the same call. What this
    // fills is used only once its copies are done (publishUploadedLoads, in a later frame): a load's uploads need no wait
    // for the frames, on the transfer queue when there is one, and the frames never wait for them.
    bool AssetLoader::Impl::finishReadyLoads(bool budgeted)
    {
        const auto deadline = std::chrono::steady_clock::now() + LOAD_BUDGET;
        auto overBudget = [budgeted, deadline]()
        { return budgeted && std::chrono::steady_clock::now() >= deadline; };

        publishUploadedLoads();
        renderer->ctx.beginLoadUploads();
        std::vector<MeshHandle> meshes;
        std::vector<EnvironmentHandle> uploadedEnvironments;
        std::vector<PendingTextureLoad> filledTextures;

        for (size_t i = 0; i < pendingMeshes.size() && !overBudget();)
        {
            if (pendingMeshes[i].job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE)
            {
                ++i;
                continue;
            }
            PendingMeshLoad load = std::move(pendingMeshes[i]);
            pendingMeshes.erase(pendingMeshes.begin() + static_cast<std::ptrdiff_t>(i));
            finishMeshLoad(load);
            if (renderer->isLive(load.handle) && renderer->registry.meshes[load.handle.index].state == MeshState::LOADING)
                meshes.push_back(load.handle);
            // Its textures took their levels' bytes with them (loadMaterial); the rest of the result is freed on a worker.
            const uint64_t held = load.job->ticket.bytes.exchange(0);
            freeOnWorker(std::move(load.job), held - std::min(held, load.movedBytes));
        }

        for (size_t i = 0; i < pendingEnvironments.size() && !overBudget();)
        {
            if (pendingEnvironments[i].job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE)
            {
                ++i;
                continue;
            }
            PendingEnvironmentLoad load = std::move(pendingEnvironments[i]);
            pendingEnvironments.erase(pendingEnvironments.begin() + static_cast<std::ptrdiff_t>(i));
            renderer->setEnvironmentImage(load.handle, load.path, load.job->raw);
            uploadedEnvironments.push_back(load.handle);
            const uint64_t held = load.job->ticket.bytes.exchange(0);
            freeOnWorker(std::move(load.job), held);
        }

        // At least one part of a texture a frame, so meshes that use up the budget frame after frame do not hold textures
        // back. A texture is filled in parts of TEXTURE_FILL_PART, so one big texture does not make a long frame either
        // (P24), and the ones begun go first, so few are half done at a time. An entry leaves the map while it is filled:
        // a fill that fails destroys its texture. One that is not done comes back with its levels; one that is comes back
        // without them, and waits for its copies.
        bool filledOne = false;
        for (const bool begun : {true, false})
        {
            for (auto it = pendingTextures.begin(); it != pendingTextures.end() && (!filledOne || !overBudget());)
            {
                const PendingTextureLoad &entry = it->second;
                const bool started = entry.progress.mip > 0 || entry.progress.row > 0;
                if (entry.filled || started != begun || (entry.job && entry.job->ticket.state.load(std::memory_order_acquire) != LoadState::DONE))
                {
                    ++it;
                    continue;
                }
                PendingTextureLoad load = std::move(it->second);
                it = pendingTextures.erase(it);
                // A part that copied nothing found the staging ring full: the rest waits for a later frame.
                bool finished = false;
                bool moved = true;
                while (!finished && moved && (!filledOne || !overBudget()))
                {
                    const TextureFillProgress before = load.progress;
                    finished = finishTextureLoad(load, budgeted ? TEXTURE_FILL_PART : UINT64_MAX);
                    moved = load.progress.mip != before.mip || load.progress.row != before.row;
                    filledOne = true;
                }
                if (!finished)
                {
                    filledTextures.push_back(std::move(load));
                    continue;
                }
                // A texture has its own job (loadTexture) or its glTF's levels (loadMaterial), never both.
                if (load.job)
                {
                    const uint64_t held = load.job->ticket.bytes.exchange(0);
                    freeOnWorker(std::move(load.job), held);
                }
                else
                    freeOnWorker(std::move(load.levels), load.bytes);
                if (renderer->ctx.isAlive(load.handle))
                    filledTextures.push_back({.handle = load.handle, .path = std::move(load.path), .filled = true});
                else
                    renderer->textureLoaded(load.handle);
            }
        }

        const uint64_t value = renderer->ctx.endLoadUploads();
        loadUploadValue = value;
        for (MeshHandle mesh : meshes)
            uploadingMeshes.emplace_back(mesh, value);
        for (EnvironmentHandle environment : uploadedEnvironments)
        {
            if (renderer->isLive(environment))
                renderer->environments->environments[environment.index].uploadValue = value;
        }
        for (PendingTextureLoad &texture : filledTextures)
        {
            texture.uploadValue = value;
            const TextureHandle handle = texture.handle;
            pendingTextures.emplace(handle, std::move(texture));
        }
        // An environment whose image is still copied bakes later, so it is still loading too.
        bool environmentsUploading = false;
        for (const Environment &environment : renderer->environments->environments)
            environmentsUploading = environmentsUploading || (environment.state == BakeState::WAITING && !renderer->ctx.isLoadUploadDone(environment.uploadValue));
        return !pendingMeshes.empty() || !pendingEnvironments.empty() || !pendingTextures.empty() || !uploadingMeshes.empty() || environmentsUploading;
    }

    // A mesh and a texture are loaded once the GPU has their copies; an environment is baked then (recordEnvironmentBakes).
    // The frame's submits wait for what is published here, which is done, so they never wait for copies in flight.
    void AssetLoader::Impl::publishUploadedLoads()
    {
        renderer->ctx.updateLoadUploadsDone();
        for (auto it = pendingTextures.begin(); it != pendingTextures.end();)
        {
            if (!it->second.filled || !renderer->ctx.isLoadUploadDone(it->second.uploadValue))
            {
                ++it;
                continue;
            }
            // syncGpuScene checks the entities that wait for textures only after one finished.
            renderer->textureLoaded(it->first);
            it = pendingTextures.erase(it);
        }
        std::erase_if(uploadingMeshes, [this](const std::pair<MeshHandle, uint64_t> &mesh)
                      {
                          if (!renderer->ctx.isLoadUploadDone(mesh.second))
                              return false;
                          // A mesh destroyed meanwhile (its handle or its slot's new mesh) is left alone.
                          if (renderer->isLive(mesh.first) && renderer->registry.meshes[mesh.first.index].state == MeshState::LOADING)
                              renderer->registry.meshes[mesh.first.index].state = MeshState::READY;
                          return true; });
    }

    AssetLoader::AssetLoader(Renderer &renderer, const AssetLoaderConfig &config) : impl(std::make_unique<Impl>(renderer, config)) {}

    AssetLoader::~AssetLoader() = default;

    MeshHandle AssetLoader::loadMesh(const std::string &path)
    {
        return impl->attached("loadMesh") ? impl->loadMesh(path) : MeshHandle{};
    }

    MeshHandle AssetLoader::loadGLTF(const std::string &path)
    {
        return loadMesh(path);
    }

    TextureHandle AssetLoader::loadTexture(const std::string &path, Format format)
    {
        return impl->attached("loadTexture") ? impl->loadTexture(path, format) : TextureHandle{};
    }

    EnvironmentHandle AssetLoader::loadEnvironment(const std::string &path)
    {
        return impl->attached("loadEnvironment") ? impl->loadEnvironment(path) : EnvironmentHandle{};
    }

    void AssetLoader::waitForLoads()
    {
        impl->waitForLoads();
    }

    LoadStats AssetLoader::getLoadStats() const
    {
        return impl->getLoadStats();
    }

    std::string AssetLoader::resolveAssetPath(const std::string &path) const
    {
        return impl->resolveAssetPath(path);
    }

    AssetLoaderConfig AssetLoader::getConfig() const
    {
        return impl->config;
    }
}
