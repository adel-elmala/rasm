// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/picking.h"
#include "rasm/types.h"
#include "rasm/view.h"
#include "renderer/gpuTable.h"
#include "renderer/uberMaterialTypes.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <vector>

namespace rasm
{

    // Picks one frame records at most, side by side in one row of the slot's targets.
    constexpr uint32_t              MAX_FRAME_PICKS         = 8;
    constexpr uint32_t              MAX_PICK_ROW_WIDTH      = 2048;

    // Which entity's sub-mesh an instance row draws.
    struct InstanceOwner
    {
        uint32_t                    entityIndex             = 0;
        uint32_t                    entityGeneration        = 0;    // 0: no entity yet
        uint32_t                    subMesh                 = 0;
    };

    // The owner of every instance row as each frame slot's copy of the instance table holds it. Rows are given to
    // another entity at once (each slot has its own copy), so a pick reads its rows' owners from the slot it drew in,
    // before that slot's next sync.
    class InstanceOwners
    {
    public:
        void                        reset(uint32_t rows);
        // syncGpuScene gives rows first .. first + count - 1 to the entity's sub-meshes 0 .. count - 1.
        void                        assign(uint32_t first, uint32_t count, EntityHandle entity);
        // Before the slot's instance rows go up: the slot takes the rows assigned since its last sync.
        void                        sync(uint32_t slot);
        InstanceOwner               get(uint32_t slot, uint32_t row) const;

    private:
        std::vector<InstanceOwner>  current;
        std::vector<InstanceOwner>  slots[MAX_FRAMES_IN_FLIGHT];
        std::vector<RowRange>       pending[MAX_FRAMES_IN_FLIGHT];
    };

    // One pixel of a pick that shows something, as the pick pass wrote it (rasm.slang's pickMain).
    struct PickSample
    {
        uint32_t                    row                     = 0;
        uint32_t                    feature                 = 0;
        uint32_t                    flags                   = 0;
        uint32_t                    normal                  = 0;    // packPickNormal
        glm::vec3                   position                = glm::vec3(0.0f);  // render space
        uint32_t                    x                       = 0;    // in the view
        uint32_t                    y                       = 0;
    };

    // The pixels of one (row, feature): their count and the one nearest the rect's centre, then nearest the camera.
    struct PickGroup
    {
        PickSample                  nearest;
        uint32_t                    pixels                  = 0;
        float                       centreDistance          = 0.0f; // squared, in pixels
        float                       cameraDistance          = 0.0f;
    };

    enum class PickState : uint8_t
    {
        WAITING,                                                    // for an endFrame to record it
        RECORDED,                                                   // in this frame's commands
        SUBMITTED,                                                  // in a submitted frame: read once its fence passes
        DONE,                                                       // hits read
        FAILED,
    };

    struct PickRequest
    {
        PickHandle                  handle;
        bool                        byView                  = false;
        View                        view;                           // byView: resolved to its scene target when recorded
        SceneHandle                 scene;
        CameraHandle                camera;
        RenderTargetHandle          target;
        PickDesc                    desc;
        PickState                   state                   = PickState::WAITING;
        uint32_t                    slot                    = 0;
        uint64_t                    fenceValue              = 0;
        uint32_t                    atlasX                  = 0;    // its rect's column in the slot's targets
        ViewPixels                  rect;                           // inside the view
        glm::dvec3                  origin                  = glm::dvec3(0.0);  // its view's render space
        std::vector<PickHit>        hits;                           // keeps its capacity when the request is reused
    };

    // A frame slot's pick resources, grown and kept: the targets (ids, positions, depth), the culling buffer that the
    // frame's picks share one after another, and the host copy of the targets' used corner.
    struct PickSlot
    {
        TextureHandle               ids;
        TextureHandle               positions;
        TextureHandle               depth;
        uint32_t                    width                   = 0;
        uint32_t                    height                  = 0;
        BufferHandle                cull;
        uint32_t                    cullCapacity            = 0;    // draws
        BufferHandle                readback;
        uint64_t                    readbackSize            = 0;
        uint32_t                    usedWidth               = 0;    // the copied corner of the last frame that recorded picks
        uint32_t                    usedHeight              = 0;
    };

    struct Picks
    {
        HandlePool                  handles                 = HandlePool(1u << 16);
        std::vector<PickRequest>    requests;
        std::vector<PickRequest>    spare;                          // taken requests, whose hit vectors keep their capacity
        PickSlot                    slots[MAX_FRAMES_IN_FLIGHT];
        PipelineHandle              pipelines[3]            = {};   // opaque, MASK, BLEND
        InstanceOwners              owners;
        std::vector<uint32_t>       texels;                         // scratch: a slot's copied ids then positions
        std::vector<PickSample>     samples;                        // scratch
        std::vector<PickGroup>      groups;                         // scratch
    };

    // Clip space to the rect's own clip space: the rect's pixels of a view of width x height fill -1..1, so the planes
    // of (this x projection x view) are the frustum through the rect.
    glm::dmat4                      getPickClipMatrix(const ViewPixels& rect, uint32_t width, uint32_t height);
    // The pick's PICK_SKIP_* mask and minAlpha (in 1/255 steps), for its draws' push constant.
    uint32_t                        getPickArgument(const PickDesc& desc);
    glm::vec3                       unpackPickNormal(uint32_t packed);
    // Groups the samples by (row, feature), sorted nearest the rect's centre first, then nearest the camera, then by
    // row. samples is sorted in place; groups keeps its capacity.
    void                            groupPickSamples(std::vector<PickSample>& samples, const ViewPixels& rect, std::vector<PickGroup>& groups);

}
// clang-format on
