// clang-format off
#pragma once

#include "rasm/assetLoader.h"
#include "rasm/renderer.h"
#include "assets/loadJobs.h"
#include "core/workerPool.h"
#include "renderer/loadHooks.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tinygltf { class Model; }

namespace rasm
{

    // The loader's state: the worker pool, the loads in flight, the path caches. The renderer calls it through
    // LoadHooks; it reaches the renderer's objects as its friend. renderer is null once the renderer is gone (detach).
    struct AssetLoader::Impl final : LoadHooks
    {
        // Each load's jobs run on the worker pool and write its job struct; finishLoads reads it once its ticket is done.
        struct PendingMeshLoad
        {
            MeshHandle                          handle;
            std::string                         path;
            std::shared_ptr<MeshLoadJob>        job;
            uint64_t                            movedBytes  = 0;    // of its budget bytes, those its textures took (finishMeshLoad)
        };

        // A load decodes on a worker (job); a glTF texture arrives decoded with its mesh (levels, which hold bytes of
        // the load budget). Either is filled by finishLoads under its budget.
        struct PendingTextureLoad
        {
            TextureHandle                       handle;
            std::string                         path;
            std::shared_ptr<TextureLoadJob>     job;
            std::shared_ptr<const TextureRaw>   levels;
            uint64_t                            bytes       = 0;
            TextureFillProgress                 progress;               // the parts filled so far (P24)
            bool                                filled      = false;    // its copies are on their way: it is loaded once the load uploads reach uploadValue
            uint64_t                            uploadValue = 0;
        };

        struct PendingEnvironmentLoad
        {
            EnvironmentHandle                   handle;
            std::string                         path;
            std::shared_ptr<EnvironmentLoadJob> job;
        };

                                    Impl(Renderer& renderer, const AssetLoaderConfig& config);
                                    ~Impl();

        void                        finishLoads() override;
        void                        waitForLoads() override;
        LoadStats                   getLoadStats() const override;
        void                        forget(MeshHandle mesh) override;
        void                        forget(TextureHandle texture) override;
        void                        forget(EnvironmentHandle environment) override;
        void                        detach() override;

        bool                        attached(const char* caller) const;
        std::string                 resolveAssetPath(const std::string& path) const;
        MeshHandle                  loadMesh(const std::string& path);
        TextureHandle               loadTexture(const std::string& path, Format format);
        EnvironmentHandle           loadEnvironment(const std::string& path);
        void                        finishMeshLoad(PendingMeshLoad& load);
        MaterialHandle              loadMaterial(const tinygltf::Model& model, int materialIndex, GltfImportCache& cache);
        bool                        finishTextureLoad(PendingTextureLoad& load, uint64_t maxBytes);
        bool                        fillTextureLevels(TextureHandle texture, const TextureRaw& raw);
        bool                        fillTextureLevels(TextureHandle texture, const TextureRaw& raw, TextureFillProgress& progress, uint64_t maxBytes);
        void                        finishLoads(bool wait);
        bool                        finishReadyLoads(bool budgeted);
        void                        publishUploadedLoads();
        void                        freeOnWorker(std::shared_ptr<const void> result, uint64_t bytes);
        void                        dropPendingLoad(LoadTicket& ticket, std::shared_ptr<const void> result);
        void                        dropAll();

        Renderer*                                                               renderer            = nullptr;
        AssetLoaderConfig                                                       config              = {};
        std::unique_ptr<WorkerPool>                                             workers;
        std::vector<PendingMeshLoad>                                            pendingMeshes;
        std::unordered_map<TextureHandle, PendingTextureLoad, HandleHash>       pendingTextures;
        std::vector<PendingEnvironmentLoad>                                     pendingEnvironments;
        std::vector<std::pair<MeshHandle, uint64_t>>                            uploadingMeshes;    // finished, READY once the load uploads reach the value
        uint64_t                                                                loadUploadValue     = 0;    // what the last finishReadyLoads uploaded is on the GPU at it
        std::unordered_map<std::string, MeshHandle>                             loadedMeshes;       // by resolved path
        std::unordered_map<std::string, TextureHandle>                          loadedTextures;     // by resolved path and format
        std::vector<std::string>                                                textureCacheKeys;   // by texture index: its loadedTextures key
        std::vector<std::string>                                                meshPaths;          // by mesh index: its loadedMeshes key
    };

}
// clang-format on
