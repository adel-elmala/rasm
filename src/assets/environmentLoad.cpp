#include "assets/assetLoaderImpl.h"
#include "assets/loadJobs.h"
#include "core/workerPool.h"

#include "glm/gtc/packing.hpp"
#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rasm
{
    namespace
    {
        // The largest finite half float. Brighter texels (the sun's core) would become infinity.
        constexpr float MAX_HALF = 65504.0f;

        // Runs on a worker thread: it must not touch the renderer. RGBA16F halves the memory of the floats stb_image
        // gives, and is what the bake reads.
        EnvironmentRaw decodeEnvironment(const std::string &path)
        {
            ZoneScoped;

            int width, height, channels;
            float *data = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
            if (!data)
                return {};
            EnvironmentRaw raw{.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
            const size_t count = static_cast<size_t>(width) * height * 4;
            raw.pixels.resize(count);
            for (size_t i = 0; i < count; ++i)
                raw.pixels[i] = glm::packHalf1x16(std::clamp(data[i], 0.0f, MAX_HALF));
            stbi_image_free(data);
            return raw;
        }
    }

    EnvironmentHandle AssetLoader::Impl::loadEnvironment(const std::string &assetPath)
    {
        const std::string path = resolveAssetPath(assetPath);
        const auto extension = path.substr(path.find_last_of('.') + 1);
        if (extension != "hdr")
        {
            spdlog::error("loadEnvironment reads Radiance .hdr images, not .{}: {}", extension, path);
            return {};
        }
        // Only the header is read here, so a missing or broken file fails at once.
        int width, height, channels;
        if (!stbi_info(path.c_str(), &width, &height, &channels) || !stbi_is_hdr(path.c_str()))
        {
            spdlog::error("Failed to read the environment image {}", path);
            return {};
        }

        const EnvironmentHandle handle = renderer->createEnvironment(path);
        // While it decodes, the 16-byte floats stb_image gives and the 8-byte RGBA16F texels; then the texels alone.
        auto job = std::make_shared<EnvironmentLoadJob>();
        WorkerPool *pool = workers.get();
        auto decode = [job, pool, path]()
        {
            if (!job->ticket.cancelled)
                job->raw = decodeEnvironment(path);
            pool->setLoadBytes(job->ticket, job->raw.pixels.size() * sizeof(uint16_t));
            pool->finishLoad(job->ticket);
        };
        pool->push({.run = std::move(decode), .bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 24, .ticket = std::shared_ptr<LoadTicket>(job, &job->ticket)});
        pendingEnvironments.push_back({handle, path, std::move(job)});
        return handle;
    }
}
