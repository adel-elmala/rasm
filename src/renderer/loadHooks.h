// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

namespace rasm
{

    // What a Renderer asks of the AssetLoader attached to it. The loader lives in another library, so the renderer calls
    // it through this interface and never links it. Each call is rare: once a frame, or once per destroyed object.
    class LoadHooks
    {
    public:
        // At the start of beginFrame: the loads that are ready, within the frame's budget.
        virtual void                finishLoads() = 0;
        virtual void                waitForLoads() = 0;
        virtual LoadStats           getLoadStats() const = 0;
        // The renderer destroyed the object: its load, if any, is dropped, and its path leaves the cache.
        virtual void                forget(MeshHandle mesh) = 0;
        virtual void                forget(TextureHandle texture) = 0;
        virtual void                forget(EnvironmentHandle environment) = 0;
        // The renderer is destroyed first: every load is dropped, and the loader refuses every call from now on.
        virtual void                detach() = 0;

    protected:
                                    ~LoadHooks() = default;
    };

}
// clang-format on
