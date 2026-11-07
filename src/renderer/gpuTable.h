// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{

    class RenderContext;

    struct RowRange
    {
        uint32_t                first           = 0;
        uint32_t                count           = 0;
    };

    void                        mergeRows(std::vector<uint32_t>& rows, std::vector<RowRange>& ranges);

    class GpuTable
    {
    public:
        bool                    create(RenderContext& ctx, const std::string& name, uint32_t rowSize, uint32_t capacity);
        void                    setRow(uint32_t row, const void* data);
        bool                    upload(RenderContext& ctx, uint32_t frame);
        BufferHandle            buffer(uint32_t frame) const;
        uint32_t                capacity() const;

    private:
        BufferHandle            buffers[MAX_FRAMES_IN_FLIGHT]   = {};
        std::vector<uint8_t>    shadow;
        std::vector<uint8_t>    dirtySlots;
        std::vector<uint32_t>   dirtyRows[MAX_FRAMES_IN_FLIGHT];
        std::vector<RowRange>   ranges;
        uint32_t                rowSize                         = 0;
        uint32_t                rowCapacity                     = 0;
    };

}
// clang-format on
