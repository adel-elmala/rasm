// clang-format off
#pragma once

#include <cstdint>

namespace rasm
{

    constexpr uint32_t COLORMAP_SIZE = 256;

    enum class ColormapPreset : uint32_t
    {
        VIRIDIS,
        TURBO,
        MAGMA,
        INFERNO,
        PLASMA,
        CIVIDIS,
        GRAY,

        COUNT,
    };

    struct ScalarRange
    {
        float           min         = 0.0f;
        float           max         = 1.0f;
        bool            log         = false;        // both ends above 0
        bool            clamp       = true;         // false: values outside are not drawn
    };

}
// clang-format on
