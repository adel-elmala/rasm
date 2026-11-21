// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/scene.h"
#include "rasm/types.h"
#include "rasm/material.h"
#include "renderer/blendSort.h"
#include "renderer/gpuTable.h"
#include "renderer/uberMaterialTypes.h"

#include <cstdint>
#include <vector>

namespace rasm
{

    // The words of the cull stats buffer. Must match the STATS_* constants in shaders/passes/cull.slang and
    // shaders/passes/lightClusters.slang.
    constexpr uint32_t                  CULL_STATS_DRAWS                                                    = 0;
    constexpr uint32_t                  CULL_STATS_TRIANGLES                                                = 1;    // low, high
    constexpr uint32_t                  CULL_STATS_LOD0_TRIANGLES                                           = 3;    // low, high
    constexpr uint32_t                  CULL_STATS_LOD_DRAWS                                                = 5;    // one per LOD
    constexpr uint32_t                  CULL_STATS_LIGHTS_VISIBLE                                           = CULL_STATS_LOD_DRAWS + MAX_MESH_LODS; // lightClusters.slang
    constexpr uint32_t                  CULL_STATS_LIGHTS_DROPPED                                           = CULL_STATS_LIGHTS_VISIBLE + 1;
    constexpr uint32_t                  CULL_STATS_WORDS                                                    = CULL_STATS_LIGHTS_DROPPED + 1;

    struct EntityDrawState
    {
        uint32_t                        firstInstance                                                       = 0;
        uint32_t                        instanceCount                                                       = 0;
        uint16_t                        scenes                                                              = 0;
        bool                            waitingForTextures                                                  = false;
    };

    struct DrawRun
    {
        AlphaMode                       alphaMode                                                           = AlphaMode::NONE;
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
        uint32_t                        face                                                                = 0;    // of a cube target
        SceneShadow                     shadows[MAX_VIEW_SHADOWS]                                           = {};   // a depth-only view's own shadow is shadows[0]
        uint32_t                        shadowCount                                                         = 0;
        bool                            depthOnly                                                           = false;
        bool                            mirrored                                                            = false;    // its view matrix mirrors the world (a cube face), so front faces turn clockwise
        BufferHandle                    visibleDraws[MAX_FRAMES_IN_FLIGHT]                                  = {};
        uint32_t                        visibleCapacity[MAX_FRAMES_IN_FLIGHT]                               = {};
        BufferHandle                    hiz[MAX_FRAMES_IN_FLIGHT]                                           = {};
        uint32_t                        hizCapacity[MAX_FRAMES_IN_FLIGHT]                                   = {};
        BufferHandle                    visibility                                                          = {};
        BufferHandle                    sortBuffers[MAX_FRAMES_IN_FLIGHT]                                   = {};   // a shaded view's: the blend sort's keys, then its copy of the draws
        uint32_t                        sortCapacity[MAX_FRAMES_IN_FLIGHT]                                  = {};
        uint32_t                        graphs                                                              = 0;    // graphs that hold it; 0 is a free slot
        bool                            ready                                                               = false;
        bool                            culled                                                              = false;
        bool                            sky                                                                 = false;    // this frame's SceneData names a sky to draw
        bool                            drawMask                                                            = false;    // this frame's pipelines for the MASK runs, or the depth-only MASK and BLEND runs, exist
        bool                            drawBlend                                                           = false;    // a shaded view's Transparent pass has a run and a pipeline
        bool                            sortBlend                                                           = false;    // the cull pass sorts the blend run
        uint32_t                        blendRun                                                            = gfx::MAX_DRAW_RUNS;
        uint32_t                        groupCount                                                          = 0;
        uint32_t                        hizLevelCount                                                       = 0;
        uint32_t                        width                                                               = 0;
        uint32_t                        height                                                              = 0;
    };

    // A shaded view's pipelines for one sample count of its target: they must match the attachments they draw into.
    constexpr uint32_t                  SAMPLE_VARIANTS                                                     = 4;        // 1, 2, 4 and 8 samples

    struct ShadedPipelines
    {
        PipelineHandle                  pipeline                                                            = {};
        PipelineHandle                  maskPipeline                                                        = {};       // alpha-to-coverage with more than one sample
        PipelineHandle                  blendPipeline                                                       = {};
        PipelineHandle                  skyPipeline                                                         = {};
    };

    struct GpuScene
    {
        GpuTable                        instances;
        GpuTable                        materials;
        BufferHandle                    sceneData[MAX_FRAMES_IN_FLIGHT]                                     = {};
        ShadedPipelines                 shaded[SAMPLE_VARIANTS]                                             = {};       // by log2 of the target's samples
        PipelineHandle                  depthPipeline                                                       = {};
        PipelineHandle                  depthMaskPipeline                                                   = {};
        PipelineHandle                  sortPipelines[BLEND_SORT_STEPS]                                     = {};
        PipelineHandle                  cullCountPipeline                                                   = {};
        PipelineHandle                  cullWritePipeline                                                   = {};
        PipelineHandle                  hizPipeline                                                         = {};
        BufferHandle                    cullStats[MAX_FRAMES_IN_FLIGHT]                                     = {};
        SceneView                       views[MAX_SCENE_VIEWS];
        DrawTotals                      drawTotals[MAX_FRAMES_IN_FLIGHT]                                    = {};
        std::vector<EntityDrawState>    entities;
        std::vector<CullMode>           instanceCullModes;
        std::vector<AlphaMode>          instanceAlphaModes;
        uint32_t                        instanceCount                                                       = 0;
        std::vector<EntityHandle>       entitiesWaitingForMesh;
        std::vector<EntityHandle>       entitiesWaitingForTextures;
        bool                            texturesFinished                                                    = false;
        SceneDrawList                   drawLists[MAX_SCENES];
    };

}
// clang-format on
