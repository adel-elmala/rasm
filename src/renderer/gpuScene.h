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

    struct SceneDrawList
    {
        std::vector<gfx::DrawInfo>      draws;
        std::vector<DrawRun>            runs;
        BufferHandle                    buffers[MAX_FRAMES_IN_FLIGHT]                                       = {};
        uint32_t                        bufferCapacity[MAX_FRAMES_IN_FLIGHT]                                = {};
        uint8_t                         staleSlots                                                          = 0;
        bool                            rebuild                                                             = false;
    };

    struct GpuScene
    {
        GpuTable                        instances;
        GpuTable                        materials;
        BufferHandle                    sceneData[MAX_FRAMES_IN_FLIGHT]                                     = {};
        PipelineHandle                  pipeline                                                            = {};
        PipelineHandle                  cullCountPipeline                                                   = {};
        PipelineHandle                  cullWritePipeline                                                   = {};
        CommandBufferHandle             cullCommandBuffers[MAX_FRAMES_IN_FLIGHT]                            = {};
        BufferHandle                    drawCounts[MAX_FRAMES_IN_FLIGHT]                                    = {};
        BufferHandle                    visibleDraws[MAX_FRAMES_IN_FLIGHT][MAX_SCENE_DRAWS_PER_FRAME]       = {};
        uint32_t                        visibleCapacity[MAX_FRAMES_IN_FLIGHT][MAX_SCENE_DRAWS_PER_FRAME]    = {};
        uint32_t                        cullGroupCounts[MAX_SCENE_DRAWS_PER_FRAME]                          = {};
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
