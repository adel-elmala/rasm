// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{

    // The capture target's side: a resource is scaled to fit it, its aspect kept.
    constexpr uint32_t                  INSPECT_SIZE                                    = 1024;

    // A resource of the frame's graph that the Buffers section lists.
    struct InspectEntry
    {
        std::string                     label;                                                      // the list's text; the Inspect pass is "Inspect <label>"
        std::string                     target;                                                     // Inspection::target
        InspectKind                     kind                                            = InspectKind::COLOR;
        uint32_t                        attachment                                      = 0;
        uint32_t                        face                                            = 0;        // HIZ, LIGHT_CLUSTERS: the view's
        RenderTargetHandle              renderTarget;
        uint32_t                        view                                            = UINT32_MAX;   // the scene view that draws it, or whose Hi-Z or clusters it is
    };

    struct Inspector
    {
        std::vector<InspectEntry>       entries;                                                    // of the graph that runs this frame, while the section is open
        uint32_t                        active                                          = UINT32_MAX;   // the entry this frame's Inspect pass copies
        bool                            noUiPass                                        = false;    // the graph has no UI pass, so nothing is copied
        RenderTargetHandle              capture;                                                    // made on the first copy
        BufferHandle                    params[MAX_FRAMES_IN_FLIGHT]                    = {};       // gfx::InspectParams
        PipelineHandle                  pipeline;
        bool                            pipelineTried                                   = false;    // since the last pick: a shader error is not retried every frame
        bool                            shown                                           = false;    // the Buffers section was open in the last UI
        bool                            openBuffers                                     = false;    // setInspection opens the Buffers section in the next UI
        bool                            openTables                                      = false;    // and the GPU tables section
        bool                            openRows                                        = false;    // and the entity's instance rows in it
    };

    // The part of the capture target a source of this size fills: scaled to fit size x size, aspect kept, at least 1.
    glm::uvec2                          getInspectRegion(glm::uvec2 source, uint32_t size);
    // The size of a Hi-Z level, as getHizLevel in shaders/lib/occlusionCulling.slang finds it.
    glm::uvec2                          getHizLevelSize(glm::uvec2 viewport, uint32_t level);

}
// clang-format on
