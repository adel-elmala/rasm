// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/scene.h"
#include "rasm/types.h"
#include "renderer/gpuTable.h"
#include "renderer/uberMaterialTypes.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // The words of the cull stats buffer. Must match the STATS_* constants in shaders/passes/cull.slang.
    constexpr uint32_t                  CULL_STATS_DRAWS                                                    = 0;
    constexpr uint32_t                  CULL_STATS_TRIANGLES                                                = 1;    // low, high
    constexpr uint32_t                  CULL_STATS_LOD0_TRIANGLES                                           = 3;    // low, high
    constexpr uint32_t                  CULL_STATS_LOD_DRAWS                                                = 5;    // one per LOD
    constexpr uint32_t                  CULL_STATS_WORDS                                                    = CULL_STATS_LOD_DRAWS + MAX_MESH_LODS;

    struct EntityDrawState
    {
        uint32_t                        firstInstance                                                       = 0;
        uint32_t                        instanceCount                                                       = 0;
        uint16_t                        scenes                                                              = 0;
        bool                            waitingForTextures                                                  = false;
    };

    struct DrawRun
    {
        CullMode                        cullMode                                                            = CullMode::NONE;
        uint32_t                        first                                                               = 0;
        uint32_t                        count                                                               = 0;
    };

    struct DrawTotals
    {
        uint32_t                        draws                                                               = 0;
        uint64_t                        triangles                                                           = 0;
        uint32_t                        unculledDraws                                                       = 0;
        uint64_t                        unculledTriangles                                                   = 0;
    };

    struct SceneDrawList
    {
        std::vector<gfx::DrawInfo>      draws;
        std::vector<DrawRun>            runs;
        uint64_t                        triangleCount                                                       = 0;
        BufferHandle                    buffers[MAX_FRAMES_IN_FLIGHT]                                       = {};
        uint32_t                        bufferCapacity[MAX_FRAMES_IN_FLIGHT]                                = {};
        uint8_t                         staleSlots                                                          = 0;
        bool                            rebuild                                                             = false;
    };

    struct SceneView
    {
        SceneHandle                     scene;
        CameraHandle                    camera;
        RenderTargetHandle              renderTarget;
        BufferHandle                    visibleDraws[MAX_FRAMES_IN_FLIGHT]                                  = {};
        uint32_t                        visibleCapacity[MAX_FRAMES_IN_FLIGHT]                               = {};
        BufferHandle                    hiz[MAX_FRAMES_IN_FLIGHT]                                           = {};
        uint32_t                        hizCapacity[MAX_FRAMES_IN_FLIGHT]                                   = {};
        BufferHandle                    visibility                                                          = {};
        uint32_t                        graphs                                                              = 0;    // graphs that hold it; 0 is a free slot
        bool                            ready                                                               = false;
        bool                            culled                                                              = false;
        uint32_t                        groupCount                                                          = 0;
        uint32_t                        hizLevelCount                                                       = 0;
        uint32_t                        width                                                               = 0;
        uint32_t                        height                                                              = 0;
    };

    struct GpuScene
    {
        GpuTable                        instances;
        GpuTable                        materials;
        BufferHandle                    sceneData[MAX_FRAMES_IN_FLIGHT]                                     = {};
        PipelineHandle                  pipeline                                                            = {};
        PipelineHandle                  cullCountPipeline                                                   = {};
        PipelineHandle                  cullWritePipeline                                                   = {};
        PipelineHandle                  hizPipeline                                                         = {};
        BufferHandle                    cullStats[MAX_FRAMES_IN_FLIGHT]                                     = {};
        SceneView                       views[MAX_SCENE_VIEWS];
        DrawTotals                      drawTotals[MAX_FRAMES_IN_FLIGHT]                                    = {};
        std::vector<EntityDrawState>    entities;
        std::vector<CullMode>           instanceCullModes;
        uint32_t                        instanceCount                                                       = 0;
        std::vector<EntityHandle>       entitiesWaitingForMesh;
        std::vector<EntityHandle>       entitiesWaitingForTextures;
        bool                            texturesFinished                                                    = false;
        SceneDrawList                   drawLists[MAX_SCENES];
    };

}
// clang-format on
