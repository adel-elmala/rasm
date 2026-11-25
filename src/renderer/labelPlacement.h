// clang-format off
#pragma once

#include "glm/glm.hpp"

#include <cstdint>
#include <vector>

namespace rasm
{

    // A label of a view's frame: its slot, its box on screen in pixels, and whether its anchor is in front and near the
    // screen at all.
    struct LabelCandidate
    {
        uint32_t                    slot                = 0;
        float                       priority            = 0.0f;
        glm::vec2                   low                 = glm::vec2(0.0f);
        glm::vec2                   high                = glm::vec2(0.0f);
        bool                        inView              = false;
        glm::vec4                   probe               = glm::vec4(0.0f);  // its anchor's test in the Label Depth pass (gfx::SceneData::labelProbesPtr)
    };

    // A view's labels over frames, by slot: their fade and whether they were placed in the last frame. Its vectors
    // keep their capacity, so a steady frame allocates nothing.
    struct LabelPlacement
    {
        std::vector<float>          alpha;                                  // 0 hidden to 1 shown
        std::vector<uint32_t>       stamps;                                 // the slot's stamp alpha belongs to: a new label starts hidden
        std::vector<uint8_t>        placed;                                 // placed in the last frame
        std::vector<uint32_t>       order;                                  // scratch: the candidates by placing order
        std::vector<uint64_t>       cells;                                  // scratch: the screen's cells a placed box covers, a bit each
    };

    // Square cells of the screen collision marks: two boxes collide when they share a cell.
    constexpr float                 LABEL_CELL_PIXELS   = 8.0f;
    // Seconds a label takes to fade in or out.
    constexpr float                 LABEL_FADE_SECONDS  = 0.25f;
    // A label shows while its anchor is in front of the depth at one of the 3 x 3 pixels around it, within this many
    // pixels' size at the anchor: the ground it stands on never hides it, a thing in front of it does, whole.
    constexpr double                LABEL_DEPTH_PIXELS  = 4.0;
    // LabelCandidate::probe.w, the Label Depth pass's test of a slot. Must match shaders/passes/labelDepth.slang.
    constexpr float                 LABEL_PROBE_NONE    = 0.0f;     // not in the view: the slot is left as it is
    constexpr float                 LABEL_PROBE_DEPTH   = 1.0f;     // tested against the depth
    constexpr float                 LABEL_PROBE_OVERLAY = 2.0f;     // an overlay's: always shown
    constexpr float                 LABEL_PROBE_RESET   = 4.0f;     // added when the label was hidden: its fade starts at its test's answer

    // Places the candidates greedily on a width x height screen: the higher priority first, then among equals the labels
    // placed last frame, then by slot, so equal labels keep the same winner and nothing flickers. A label is placed when
    // its anchor is in view and none of its cells is taken; each label then fades toward placed (1) or not (0) by step,
    // except on the state's first placement, which shows them at once. slotStamps holds each slot's stamp: a slot whose
    // stamp changed holds a new label, which starts hidden.
    void                            placeLabels(LabelPlacement& state, LabelCandidate* candidates, uint32_t count, const uint32_t* slotStamps, uint32_t slotCount,
                                                uint32_t width, uint32_t height, float step);

}
// clang-format on
