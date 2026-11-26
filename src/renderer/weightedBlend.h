// clang-format off
#pragma once

#include "rasm/render/resource.h"
#include "rasm/types.h"

#include "glm/vec2.hpp"

#include <cstdint>

namespace rasm
{
    constexpr Format                    WEIGHTED_ACCUM_FORMAT                   = Format::R16G16B16A16_SFLOAT;
    constexpr Format                    WEIGHTED_COVERAGE_FORMAT                = Format::R8_UNORM;
    constexpr Format                    WEIGHTED_MOTION_FORMAT                  = Format::R16G16B16A16_SFLOAT;
    constexpr float                     WEIGHTED_RANGE_SECONDS                  = 0.25f;
    constexpr const char*               WEIGHTED_DEFINES                        = "#define RASM_ALPHA_BLEND 1\n#define RASM_WEIGHTED 1\n";
    constexpr const char*               WEIGHTED_ADDITIVE_DEFINES               = "#define RASM_ALPHA_BLEND 1\n#define RASM_ADDITIVE 1\n";

    struct WeightedRange
    {
        float                           nearest                                 = 0.0f;
        float                           farthest                                = 0.0f;
        float                           targetNearest                           = 0.0f;
        float                           targetFarthest                          = 0.0f;
        bool                            orthographic                            = false;
        bool                            valid                                   = false;
    };

    RenderTargetDesc                    getWeightedTargetDesc(const RenderTargetDesc& sceneTarget);
    const char*                         getCompositeDefines(uint32_t samples, bool sampleRateShading);
    uint32_t                            getDepthBits(float depth);
    float                               fromDepthBits(uint32_t bits);
    void                                stepWeightedRange(WeightedRange& range, float nearest, float farthest, bool orthographic, float seconds);
    glm::vec2                           getWeightedDepths(const WeightedRange& range);
}
// clang-format on
