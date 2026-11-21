// clang-format off
#pragma once

#include "rasm/animation.h"
#include "rasm/handle.h"
#include "rasm/types.h"
#include "renderer/gpuTable.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // A mesh's animations in the animation pool.
    struct MeshAnimation
    {
        uint64_t                        address                                         = 0;        // its gfx::AnimationMeshData
        uint32_t                        nodeCount                                       = 0;
        std::vector<int32_t>            subMeshTransforms;                                          // per sub-mesh: its row among the entity's transforms, -1: static
        uint32_t                        transformCount                                  = 0;        // the animated sub-meshes
    };

    struct EntityAnimation
    {
        uint32_t                        row                                             = UINT32_MAX;   // in the animated entity table, from its first play
        uint32_t                        firstTransform                                  = 0;
        uint32_t                        firstNode                                       = 0;
        bool                            playing                                         = false;
        bool                            pending                                         = false;    // asked while its mesh loads
        uint32_t                        resetSync                                       = 0;
        AnimationPlay                   play;
    };

    struct GpuAnimation
    {
        BufferHandle                    pool                                            = {};
        uint64_t                        poolUsed                                        = 0;
        std::vector<MeshAnimation>      meshes;                                                     // by mesh index
        GpuTable                        entities;
        uint32_t                        entityRows                                      = 0;
        std::vector<EntityAnimation>    entityStates;                                               // by entity index
        BufferHandle                    transforms[MAX_FRAMES_IN_FLIGHT]                = {};
        uint32_t                        transformCapacity                               = 0;
        uint32_t                        transformCount                                  = 0;
        BufferHandle                    pose                                            = {};
        uint32_t                        poseCapacity                                    = 0;        // nodes
        uint32_t                        poseCount                                       = 0;
        BufferHandle                    frames[MAX_FRAMES_IN_FLIGHT]                    = {};
        PipelineHandle                  pipeline                                        = {};
        double                          clock                                           = 0.0;
        double                          base                                            = 0.0;
    };

    uint64_t                            getAnimationPoolBytes(const AnimationRaw& animation, uint32_t subMeshCount);
    std::vector<uint8_t>                buildAnimationPoolData(const AnimationRaw& animation, const int32_t* subMeshNodes, uint32_t subMeshCount, uint64_t address);

}
// clang-format on
