#include "renderer/gpuTable.h"
#include "rasm/render/renderContext.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace rasm
{
    void mergeRows(std::vector<uint32_t> &rows, std::vector<RowRange> &ranges)
    {
        ranges.clear();
        std::sort(rows.begin(), rows.end());
        for (uint32_t row : rows)
        {
            if (!ranges.empty() && row < ranges.back().first + ranges.back().count)
                continue;
            if (!ranges.empty() && row == ranges.back().first + ranges.back().count)
                ++ranges.back().count;
            else
                ranges.push_back({row, 1});
        }
    }

    bool GpuTable::create(RenderContext &ctx, const std::string &name, uint32_t bytesPerRow, uint32_t capacity)
    {
        if (bytesPerRow == 0 || capacity == 0)
        {
            spdlog::error("GPU table '{}' needs a row size and a capacity.", name);
            return false;
        }

        rowSize = bytesPerRow;
        rowCapacity = capacity;
        shadow.assign(static_cast<size_t>(rowSize) * capacity, 0);
        dirtySlots.assign(capacity, 0);

        // One buffer per frame slot: slot f is written only after the GPU finished the last frame that read it.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            buffers[i] = ctx.createBuffer({.name = name + " " + std::to_string(i),
                                           .usage = BufferUsage::STORAGE,
                                           .size = static_cast<uint64_t>(rowSize) * capacity});
            if (!buffers[i].isValid())
                return false;
        }
        return true;
    }

    void GpuTable::setRow(uint32_t row, const void *data)
    {
        assert(row < rowCapacity);
        std::memcpy(shadow.data() + static_cast<size_t>(row) * rowSize, data, rowSize);

        // Every slot still holds the old row, so each one uploads it once, in its own frame.
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if ((dirtySlots[row] & bit) == 0)
                dirtyRows[i].push_back(row);
            dirtySlots[row] |= bit;
        }
    }

    bool GpuTable::upload(RenderContext &ctx, uint32_t frame)
    {
        auto &rows = dirtyRows[frame];
        if (rows.empty())
            return true;

        ZoneScoped;

        mergeRows(rows, ranges);
        bool uploaded = true;
        for (const auto &range : ranges)
        {
            const size_t offset = static_cast<size_t>(range.first) * rowSize;
            uploaded &= ctx.fillBuffer(buffers[frame], shadow.data() + offset, static_cast<size_t>(range.count) * rowSize, offset);
        }
        // The rows stay dirty, so this slot sends them all again in its next frame.
        if (!uploaded)
            return false;

        const uint8_t bit = static_cast<uint8_t>(1u << frame);
        for (uint32_t row : rows)
            dirtySlots[row] &= static_cast<uint8_t>(~bit);
        rows.clear();
        return uploaded;
    }

    BufferHandle GpuTable::buffer(uint32_t frame) const
    {
        return buffers[frame];
    }

    uint32_t GpuTable::capacity() const
    {
        return rowCapacity;
    }
}
