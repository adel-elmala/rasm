#include "rasm/renderer.h"
#include "renderer/gpuScene.h"
#include "renderer/loadHooks.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    bool Renderer::isLoaded(TextureHandle texture) const
    {
        return ctx.isAlive(texture) && !loadingTextures.contains(texture);
    }

    // Each material that uses the texture loses it, and its row is written again.
    void Renderer::detachTexture(const TextureHandle &texture)
    {
        if (!texture.isValid())
            return;
        bool changed = false;
        for (uint64_t i = 1; i < registry.materials.size(); ++i)
        {
            Material &material = registry.materials[i];
            if (!isLive(material.handle))
                continue;
            bool used = false;
            for (TextureHandle &slot : material.textures)
            {
                if (slot == texture)
                {
                    slot = {};
                    used = true;
                }
            }
            if (used)
            {
                writeMaterialRow(material.handle);
                changed = true;
            }
        }
        // An entity that waited for this texture may be drawable now.
        if (changed)
            gpuScene->texturesFinished = true;
    }

    // Materials sample by slot number, so each one that uses the texture loses it before the slot can take another
    // texture.
    void Renderer::destroyTexture(const TextureHandle &texture)
    {
        if (!ctx.isAlive(texture))
        {
            spdlog::error("destroyTexture: texture handle {{{}, {}}} is invalid or was destroyed.", texture.index, texture.generation);
            return;
        }

        detachTexture(texture);
        // The loader drops a load still in its pool, and the path leaves its cache.
        if (loads)
            loads->forget(texture);
        loadingTextures.erase(texture);
        ctx.destroyTexture(texture);
    }
}
