// clang-format off
#pragma once

#include "renderer/labelPlacement.h"
#include "rasm/handle.h"
#include "rasm/scene.h"
#include "rasm/types.h"
#include "rasm/material.h"
#include "core/rangeAllocator.h"
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
    constexpr uint32_t                  CULL_STATS_MESHLETS                                                 = CULL_STATS_LIGHTS_DROPPED + 1;
    constexpr uint32_t                  CULL_STATS_MESHLETS_CULLED                                          = CULL_STATS_MESHLETS + 1;
    constexpr uint32_t                  CULL_STATS_MESHLETS_WHOLE                                           = CULL_STATS_MESHLETS + 2;  // draws past a run's budget, drawn whole
    constexpr uint32_t                  CULL_STATS_LOD_TRIANGLES                                            = CULL_STATS_MESHLETS_WHOLE + 1;    // low, high: before meshlet culling
    constexpr uint32_t                  CULL_STATS_WORDS                                                    = CULL_STATS_LOD_TRIANGLES + 2;

    struct EntityDrawState
    {
        uint32_t                        firstInstance                                                       = 0;
        uint32_t                        instanceCount                                                       = 0;
        uint16_t                        scenes                                                              = 0;
        bool                            waitingForTextures                                                  = false;
        uint32_t                        movedSync                                                           = UINT32_MAX;   // GpuScene::syncCount when its rows last changed
        Transform                       syncedTransform                                                     = {};   // the entity transform the last synced frame drew
        uint32_t                        listStamp                                                           = 0;    // the draw list rebuild that last met it, so it is drawn once
        int32_t                         firstLabel                                                          = -1;   // its first label slot (GpuScene::labelSlots), -1 without labels
        uint32_t                        labelCount                                                          = 0;
    };

    struct DrawRun
    {
        AlphaMode                       alphaMode                                                           = AlphaMode::NONE;
        CullMode                        cullMode                                                            = CullMode::NONE;
        bool                            points                                                              = false;    // point cloud chunks, drawn by the point pipeline
        uint32_t                        first                                                               = 0;
        uint32_t                        count                                                               = 0;
        uint32_t                        meshletDraws                                                        = 0;    // draws whose geometry has meshlets
        uint32_t                        meshlets                                                            = 0;    // their meshlets at the LOD with the most
        uint32_t                        chunks                                                              = 0;    // their chunks of 64 at that LOD
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
        std::vector<EntityHandle>       labelEntities;                                                              // drawn entities with labels, for each view's placement
    };

    struct SceneView
    {
        glm::dmat4                      previousViewProjection                                              = glm::dmat4(1.0);  // the frame before's, world space, without jitter
        glm::dmat4                      frozenViewProjection                                                = glm::dmat4(1.0);  // world space
        glm::dvec3                      frozenOrigin                                                        = glm::dvec3(0.0);  // the render space of frozenPlanes, frozenLodDepthPlane and frozenViewPoint
        SceneHandle                     scene;
        CameraHandle                    camera;
        RenderTargetHandle              renderTarget;
        uint32_t                        face                                                                = 0;    // of a cube target
        SceneShadow                     shadows[MAX_VIEW_SHADOWS]                                           = {};   // a depth-only view's own shadow is shadows[0]
        uint32_t                        shadowCount                                                         = 0;
        bool                            depthOnly                                                           = false;
        bool                            mirrored                                                            = false;    // its view matrix mirrors the world (a cube face), so front faces turn clockwise
        BufferHandle                    visibleDraws[MAX_FRAMES_IN_FLIGHT]                                  = {};   // the draws, the meshlet draws, then culling's scratch (VisibleDrawLayout)
        uint32_t                        visibleCapacity[MAX_FRAMES_IN_FLIGHT]                               = {};
        uint32_t                        meshletCapacity[MAX_FRAMES_IN_FLIGHT]                               = {};   // meshlet draws
        uint32_t                        chunkCapacity[MAX_FRAMES_IN_FLIGHT]                                 = {};
        BufferHandle                    meshletVisibility                                                   = {};   // a bit per meshlet of an instance, kept across frames like visibility
        uint32_t                        meshletRuns                                                         = 0;    // this frame's runs drawn by meshlets
        bool                            meshShaders                                                         = false;    // this frame's meshlets: task and mesh shaders, not indirect draws
        uint32_t                        runMeshletFirst[gfx::MAX_DRAW_RUNS]                                 = {};
        uint32_t                        runMeshletSize[gfx::MAX_DRAW_RUNS]                                  = {};
        BufferHandle                    hiz[MAX_FRAMES_IN_FLIGHT]                                           = {};
        uint32_t                        hizCapacity[MAX_FRAMES_IN_FLIGHT]                                   = {};
        BufferHandle                    visibility                                                          = {};
        BufferHandle                    sortBuffers[MAX_FRAMES_IN_FLIGHT]                                   = {};   // a shaded view's: the blend sort's keys, then its copy of the draws
        uint32_t                        sortCapacity[MAX_FRAMES_IN_FLIGHT]                                  = {};
        uint32_t                        graphs                                                              = 0;    // graphs that hold it; 0 is a free slot
        bool                            ready                                                               = false;
        bool                            culled                                                              = false;
        bool                            sky                                                                 = false;    // this frame's SceneData names a sky to draw
        bool                            grid                                                                = false;    // and a grid (its camera's)
        bool                            drawMask                                                            = false;    // this frame's pipelines for the MASK runs, or the depth-only MASK and BLEND runs, exist
        bool                            drawBlend                                                           = false;    // a shaded view's Transparent pass has a run and a pipeline
        bool                            sortBlend                                                           = false;    // the cull pass sorts the blend run
        uint32_t                        blendRun                                                            = gfx::MAX_DRAW_RUNS;
        uint32_t                        groupCount                                                          = 0;
        uint32_t                        hizLevelCount                                                       = 0;
        bool                            late                                                                = false;
        DebugView                       debugView                                                           = DebugView::NONE;
        bool                            frozen                                                              = false;
        glm::vec4                       frozenPlanes[6]                                                     = {};
        float                           frozenLodPixelsPerUnit                                              = 0.0f;
        glm::vec4                       frozenLodDepthPlane                                                 = glm::vec4(0.0f);
        glm::vec4                       frozenViewPoint                                                     = glm::vec4(0.0f);
        uint32_t                        width                                                               = 0;
        uint32_t                        height                                                              = 0;
        bool                            jitter                                                              = false;    // addTaaPass: the projection takes a sub-pixel offset per frame
        uint32_t                        jitterFrame                                                         = 0;        // counts the jittered frames, for getTaaJitter
        bool                            hasPrevious                                                         = false;
        LabelPlacement                  labels;                                                                     // a shaded view's labels over frames
        std::vector<LabelCandidate>     labelCandidates;                                                            // scratch, keeps its capacity
        BufferHandle                    labelBuffers[MAX_FRAMES_IN_FLIGHT]                                  = {};   // each label slot's alpha, which the vertex stage reads; grows under the same handle
        BufferHandle                    labelProbes[MAX_FRAMES_IN_FLIGHT]                                   = {};   // each label slot's depth test (gfx::SceneData::labelProbesPtr)
        BufferHandle                    labelFades                                                          = {};   // each label slot's fade by depth, kept across frames like visibility
        uint32_t                        labelCapacity[MAX_FRAMES_IN_FLIGHT]                                 = {};
        uint32_t                        labelFadeCapacity                                                   = 0;
        uint32_t                        labelDepthFirst                                                     = 0;    // this frame's first slot for the Label Depth pass
        uint32_t                        labelDepthSlots                                                     = 0;    // and its end; 0: it does nothing
        std::vector<glm::vec4>          labelProbeData;                                                             // scratch, keeps its capacity
        gfx::SceneData                  sceneData                                                           = {};   // this frame's, as the GPU reads it: a pick starts from it
        glm::mat4                       projection                                                          = glm::mat4(1.0f);  // this frame's, without jitter
        glm::dvec3                      origin                                                              = glm::dvec3(0.0);  // this frame's render space
        uint32_t                        preparedSync                                                        = UINT32_MAX;   // GpuScene::syncCount of the frame prepareSceneView last ran in
    };

    // A shaded view's pipelines for one sample count of its target: they must match the attachments they draw into.
    constexpr uint32_t                  SAMPLE_VARIANTS                                                     = 4;        // 1, 2, 4 and 8 samples
    constexpr uint32_t                  MOTION_VARIANT                                                      = SAMPLE_VARIANTS;  // 1 sample and a MOTION_FORMAT color

    struct DebugPipelines
    {
        PipelineHandle                  pipeline                                                            = {};
        PipelineHandle                  maskPipeline                                                        = {};
        PipelineHandle                  blendPipeline                                                       = {};
        PipelineHandle                  meshPipeline                                                        = {};       // the opaque and MASK runs' meshlets, with mesh shaders
        PipelineHandle                  meshMaskPipeline                                                    = {};
    };

    struct ShadedPipelines
    {
        PipelineHandle                  pipeline                                                            = {};
        PipelineHandle                  maskPipeline                                                        = {};       // alpha-to-coverage with more than one sample
        PipelineHandle                  blendPipeline                                                       = {};
        PipelineHandle                  skyPipeline                                                         = {};
        PipelineHandle                  gridPipeline                                                        = {};
        PipelineHandle                  pointPipeline                                                       = {};       // the point run's (passes/points.slang), alpha-to-coverage with more than one sample
        PipelineHandle                  meshPipeline                                                        = {};       // the opaque and MASK runs' meshlets, with mesh shaders
        PipelineHandle                  meshMaskPipeline                                                    = {};
        DebugPipelines                  debug;
        DebugPipelines                  overdraw;
        PipelineHandle                  ghostPipeline                                                       = {};
    };

    struct GpuScene
    {
        GpuTable                        instances;
        GpuTable                        materials;
        BufferHandle                    sceneData[MAX_FRAMES_IN_FLIGHT]                                     = {};
        ShadedPipelines                 shaded[SAMPLE_VARIANTS + 1]                                         = {};       // by log2 of the target's samples, then MOTION_VARIANT
        PipelineHandle                  depthPipeline                                                       = {};
        PipelineHandle                  depthMaskPipeline                                                   = {};
        PipelineHandle                  depthMeshPipeline                                                   = {};
        PipelineHandle                  depthMeshMaskPipeline                                               = {};
        PipelineHandle                  sortPipelines[BLEND_SORT_STEPS]                                     = {};
        PipelineHandle                  cullCountPipeline                                                   = {};
        PipelineHandle                  cullWritePipeline                                                   = {};
        PipelineHandle                  hizPipeline                                                         = {};
        PipelineHandle                  labelDepthPipeline                                                  = {};
        PipelineHandle                  meshletCountPipeline                                                = {};
        PipelineHandle                  meshletWritePipeline                                                = {};
        BufferHandle                    cullStats[MAX_FRAMES_IN_FLIGHT]                                     = {};
        SceneView                       views[MAX_SCENE_VIEWS];
        DrawTotals                      drawTotals[MAX_FRAMES_IN_FLIGHT]                                    = {};
        std::vector<EntityDrawState>    entities;
        std::vector<CullMode>           instanceCullModes;
        std::vector<AlphaMode>          instanceAlphaModes;
        std::vector<uint8_t>            instancePoints;                                                             // per instance row: 1 for a point cloud chunk with a POINTS material
        RangeAllocator                  instanceRows;                                                               // a range per entity, reused at once: each slot has its own copy
        RangeAllocator                  meshletWords;                                                               // each view's meshlet visibility words, a range per instance row of a mesh with meshlets
        std::vector<uint32_t>           instanceMeshlets;                                                           // per instance row: the meshlets of its LOD with the most
        std::vector<int32_t>            instanceMeshletWords;                                                       // per instance row: its first word, -1 for none
        std::vector<uint32_t>           instanceMeshletWordCounts;
        bool                            meshletWordsFullLogged                                              = false;
        RangeAllocator                  labelSlots;                                                                 // MAX_LABELS: a range per entity with labels
        std::vector<uint32_t>           labelStamps;                                                                // per slot: counts its labels, so a view sees a new one start hidden
        bool                            labelSlotsFullLogged                                                = false;
        std::vector<EntityHandle>       entitiesWaitingForMesh;
        std::vector<EntityHandle>       entitiesWaitingForTextures;
        std::vector<EntityHandle>       movedEntities;          // rows written since the last sync: their previousWorld is the last synced matrix
        std::vector<EntityHandle>       settlingEntities;       // moved before the last sync: the next sync writes previousWorld = world
        uint32_t                        syncCount                                                           = 0;
        bool                            texturesFinished                                                    = false;
        uint32_t                        rebuildStamp                                                        = 0;
        SceneDrawList                   drawLists[MAX_SCENES];
    };

    // The device address of a view's SceneData in a frame slot's buffer; a pick's is at MAX_SCENE_VIEWS + its index.
    uint64_t                            getSceneDataAddress(RenderContext& ctx, const GpuScene& gpuScene, uint32_t frame, uint32_t view);
    // The six planes of a view-projection's frustum, normalized (an infinite far plane gives (0, 0, 0, nearZ)).
    void                                getFrustumPlanes(const glm::mat4& viewProjection, glm::vec4 planes[6]);

}
// clang-format on
