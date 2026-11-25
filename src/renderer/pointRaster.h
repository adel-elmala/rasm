// clang-format off
#pragma once

#include <cstdint>

namespace rasm
{

    struct PointRasterLayout
    {
        uint64_t                        batches                                 = 0;    // byte offsets in a view's point raster buffer, after its PointRasterHeader
        uint64_t                        pixels                                  = 0;
        uint64_t                        size                                    = 0;
    };

    uint32_t                            getPointBatchCapacity(uint32_t chunks);
    PointRasterLayout                   getPointRasterLayout(uint32_t batchCapacity, uint32_t width, uint32_t height);

}
// clang-format on
