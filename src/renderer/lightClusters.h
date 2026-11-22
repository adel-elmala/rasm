// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/light.h"
#include "rasm/scene.h"
#include "rasm/types.h"
#include "renderer/gpuTable.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // A shaded view's cluster grid: 16 x 9 screen tiles, each cut into 24 depth slices. Must match
    // shaders/lib/lightClusters.slang.
    constexpr uint32_t                  CLUSTER_TILES_X                                 = 16;
    constexpr uint32_t                  CLUSTER_TILES_Y                                 = 9;
    constexpr uint32_t                  CLUSTER_SLICES                                  = 24;
    constexpr uint32_t                  CLUSTER_COUNT                                   = CLUSTER_TILES_X * CLUSTER_TILES_Y * CLUSTER_SLICES;
    constexpr uint32_t                  CLUSTER_HEADER_WORDS                            = 4;    // the slices' near and far depth, the visible lights, a spare word

    struct SceneLightList
    {
        std::vector<uint32_t>           rows;                                                   // light table rows: the global lights, then the local ones
        uint32_t                        globalCount                                     = 0;
        BufferHandle                    buffers[MAX_FRAMES_IN_FLIGHT]                   = {};
        uint32_t                        bufferCapacity[MAX_FRAMES_IN_FLIGHT]            = {};
        uint8_t                         staleSlots                                      = 0;
        bool                            rebuild                                         = false;
    };

    struct ViewClusters
    {
        BufferHandle                    buffers[MAX_FRAMES_IN_FLIGHT]                   = {};   // one per frame slot, grown under the same handle
        uint32_t                        lightCapacity[MAX_FRAMES_IN_FLIGHT]             = {};
        bool                            active                                          = false;    // this frame's SceneData names the clusters
    };

    struct LightTotals
    {
        uint32_t                        global                                          = 0;
        uint32_t                        unclustered                                     = 0;    // local lights of views shaded without clusters
    };

    struct GpuLights
    {
        GpuTable                        lights;
        std::vector<uint16_t>           lightScenes;                                            // per light, a bit per scene that holds it
        std::vector<uint32_t>           listStamps;                                             // per light, the list rebuild that last met it
        uint32_t                        rebuildStamp                                    = 0;
        SceneLightList                  lists[MAX_SCENES];
        ViewClusters                    views[MAX_SCENE_VIEWS];
        LightTotals                     totals[MAX_FRAMES_IN_FLIGHT]                    = {};
        PipelineHandle                  lightsPipeline                                  = {};
        PipelineHandle                  tilesPipeline                                   = {};
        PipelineHandle                  clustersPipeline                                = {};
    };

    bool                                isGlobalLight(const Light& light);
    uint32_t                            buildSceneLightList(const Light* lights, const LightHandle* sceneLights, uint32_t count, std::vector<uint32_t>& rows);
    uint64_t                            getClusterBufferSize(uint32_t lightCapacity);
    float                               getClusterSliceDepth(uint32_t slice, float sliceNear, float sliceFar, bool even);
    int32_t                             getClusterSlice(float depth, float sliceNear, float sliceFar, bool even);

}
// clang-format on
