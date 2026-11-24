// clang-format off
#pragma once

#include "rasm/animation.h"
#include "rasm/handle.h"
#include "rasm/types.h"
#include "core/rangeAllocator.h"
#include "renderer/gpuTable.h"
#include "renderer/uberMaterialTypes.h"

#include <cstdint>
#include <vector>

namespace rasm
{
    struct ObjRaw;

    // A mesh's animations in the animation pool.
    struct MeshAnimation
    {
        uint64_t                        address                                         = 0;        // its gfx::AnimationMeshData
        uint64_t                        poolOffset                                      = 0;        // its range of the pool, freed with the mesh
        uint64_t                        poolBytes                                       = 0;
        uint32_t                        nodeCount                                       = 0;
        std::vector<int32_t>            subMeshTransforms;                                          // per sub-mesh: its row among the entity's transforms, -1: static
        std::vector<uint8_t>            subMeshMirrors;                                             // per sub-mesh: 1 when a clip can mirror it (a scale through 0)
        uint32_t                        transformCount                                  = 0;        // the animated sub-meshes
        uint32_t                        jointCount                                      = 0;
        uint32_t                        skinnedVertexCount                              = 0;        // its skinned and morphed sub-meshes' vertices
        uint32_t                        weightCount                                     = 0;        // its morph weights
    };

    struct EntityAnimation
    {
        uint32_t                        row                                             = UINT32_MAX;   // in the animated entity table, from its first play
        uint32_t                        firstTransform                                  = 0;
        uint32_t                        firstNode                                       = 0;
        uint32_t                        firstJoint                                      = 0;
        uint32_t                        firstSkinnedVertex                              = 0;
        uint32_t                        firstWeight                                     = 0;
        bool                            playing                                         = false;
        bool                            rest                                            = false;    // a skinned mesh that plays no clip: posed at rest
        bool                            pending                                         = false;    // asked while its mesh loads
        bool                            morphOverride                                   = false;    // setMorphWeights's weights replace the clip's
        std::vector<float>              morphWeights;                                               // setMorphWeights's
        uint32_t                        resetSync                                       = 0;
        AnimationPlay                   play;
    };

    struct GpuAnimation
    {
        BufferHandle                    pool                                            = {};
        RangeAllocator                  poolRanges;                                                 // bytes, in ranges of 16
        std::vector<MeshAnimation>      meshes;                                                     // by mesh index
        GpuTable                        entities;
        uint32_t                        entityRows                                      = 0;        // rows ever used: the passes' dispatch size
        std::vector<uint32_t>           freeRows;                                                   // rows of destroyed entities, written as stopped
        std::vector<EntityAnimation>    entityStates;                                               // by entity index
        BufferHandle                    transforms[MAX_FRAMES_IN_FLIGHT]                = {};
        uint32_t                        transformCapacity                               = 0;
        RangeAllocator                  transformRanges;
        BufferHandle                    pose                                            = {};
        uint32_t                        poseCapacity                                    = 0;        // nodes
        RangeAllocator                  poseRanges;
        BufferHandle                    joints[MAX_FRAMES_IN_FLIGHT]                    = {};       // 3 rows per joint matrix
        uint32_t                        jointCapacity                                   = 0;
        RangeAllocator                  jointRanges;
        BufferHandle                    skinnedVertices[MAX_FRAMES_IN_FLIGHT]           = {};       // what the Skin pass writes, skinned and morphed
        uint32_t                        skinnedCapacity                                 = 0;
        RangeAllocator                  skinnedRanges;
        uint32_t                        maxSkinnedVertices                              = 0;        // the most of one entity
        GpuTable                        weightOverrides;                                            // setMorphWeights's, one float per row, NaN: the clip's
        BufferHandle                    weights[MAX_FRAMES_IN_FLIGHT]                   = {};       // the morph weights the Animate pass writes
        RangeAllocator                  weightRanges;
        BufferHandle                    frames[MAX_FRAMES_IN_FLIGHT]                    = {};
        PipelineHandle                  pipeline                                        = {};
        PipelineHandle                  skinPipeline                                    = {};
        double                          clock                                           = 0.0;
        double                          base                                            = 0.0;
        uint32_t                        recordedSync                                    = UINT32_MAX;   // the last sync whose frame ran the pass
    };

    // Per morph target: the longest position change, which bounds how far a weight w moves any vertex (|w| times it).
    std::vector<float>                  getMorphRadii(const ObjRaw& raw);
    uint64_t                            getAnimationPoolBytes(const AnimationRaw& animation, uint32_t subMeshCount, uint32_t sphereCount, uint32_t radiusCount);
    std::vector<uint8_t>                buildAnimationPoolData(const AnimationRaw& animation, const gfx::AnimationSubMeshData* subMeshes, uint32_t subMeshCount,
                                                               const glm::vec4* spheres, uint32_t sphereCount, const float* morphRadii, uint32_t radiusCount,
                                                               uint64_t address);

}
// clang-format on
