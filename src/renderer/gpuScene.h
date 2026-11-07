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

    struct InstanceRange
    {
        uint32_t                        first                                   = 0;
        uint32_t                        count                                   = 0;
    };

    struct DrawRun
    {
        CullMode                        cullMode                                = CullMode::NONE;
        uint32_t                        first                                   = 0;
        uint32_t                        count                                   = 0;
    };

    struct SceneDrawList
    {
        std::vector<gfx::DrawInfo>      draws;
        std::vector<DrawRun>            runs;
        BufferHandle                    buffers[MAX_FRAMES_IN_FLIGHT]           = {};
        uint32_t                        bufferCapacity[MAX_FRAMES_IN_FLIGHT]    = {};
        uint8_t                         staleSlots                              = 0;
        bool                            rebuild                                 = false;
    };

    struct GpuScene
    {
        GpuTable                        instances;
        GpuTable                        materials;
        BufferHandle                    sceneData[MAX_FRAMES_IN_FLIGHT]         = {};
        PipelineHandle                  pipeline                                = {};
        std::vector<InstanceRange>      entityInstances;
        std::vector<CullMode>           instanceCullModes;
        uint32_t                        instanceCount                           = 0;
        std::vector<EntityHandle>       entitiesWaitingForMesh;
        SceneDrawList                   drawLists[MAX_SCENES];
    };

}
// clang-format on
