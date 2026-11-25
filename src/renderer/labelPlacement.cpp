#include "renderer/labelPlacement.h"

#include <algorithm>
#include <cmath>

namespace rasm
{
    void placeLabels(LabelPlacement &state, LabelCandidate *candidates, uint32_t count, const uint32_t *slotStamps, uint32_t slotCount, uint32_t width, uint32_t height, float step)
    {
        // A view's first placement shows its labels at once: there is nothing to fade from.
        const bool first = state.alpha.empty();
        if (state.alpha.size() < slotCount)
        {
            state.alpha.resize(slotCount, 0.0f);
            state.stamps.resize(slotCount, 0);
            state.placed.resize(slotCount, 0);
        }
        // A slot that holds a new label starts hidden and unplaced.
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t slot = candidates[i].slot;
            if (state.stamps[slot] != slotStamps[slot])
            {
                state.stamps[slot] = slotStamps[slot];
                state.alpha[slot] = 0.0f;
                state.placed[slot] = 0;
            }
        }

        state.order.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            state.order[i] = i;
        std::sort(state.order.begin(), state.order.end(), [&](uint32_t a, uint32_t b)
                  {
                      const LabelCandidate &x = candidates[a];
                      const LabelCandidate &y = candidates[b];
                      if (x.priority != y.priority)
                          return x.priority > y.priority;
                      if (state.placed[x.slot] != state.placed[y.slot])
                          return state.placed[x.slot] > state.placed[y.slot];
                      return x.slot < y.slot; });

        const auto columns = static_cast<uint32_t>(std::ceil(static_cast<float>(width) / LABEL_CELL_PIXELS));
        const auto rows = static_cast<uint32_t>(std::ceil(static_cast<float>(height) / LABEL_CELL_PIXELS));
        const size_t words = (static_cast<size_t>(columns) * rows + 63) / 64;
        state.cells.assign(words, 0);
        for (uint32_t index : state.order)
        {
            const LabelCandidate &candidate = candidates[index];
            bool free = candidate.inView && columns > 0 && rows > 0;
            // The cells the box covers, clipped to the screen.
            const auto x0 = static_cast<int32_t>(std::floor(std::max(candidate.low.x, 0.0f) / LABEL_CELL_PIXELS));
            const auto y0 = static_cast<int32_t>(std::floor(std::max(candidate.low.y, 0.0f) / LABEL_CELL_PIXELS));
            const auto x1 = std::min(static_cast<int32_t>(std::floor(candidate.high.x / LABEL_CELL_PIXELS)), static_cast<int32_t>(columns) - 1);
            const auto y1 = std::min(static_cast<int32_t>(std::floor(candidate.high.y / LABEL_CELL_PIXELS)), static_cast<int32_t>(rows) - 1);
            free = free && x0 <= x1 && y0 <= y1;
            for (int32_t y = y0; free && y <= y1; ++y)
            {
                for (int32_t x = x0; free && x <= x1; ++x)
                {
                    const size_t cell = static_cast<size_t>(y) * columns + static_cast<size_t>(x);
                    free = (state.cells[cell / 64] & (1ull << (cell % 64))) == 0;
                }
            }
            if (free)
            {
                for (int32_t y = y0; y <= y1; ++y)
                {
                    for (int32_t x = x0; x <= x1; ++x)
                    {
                        const size_t cell = static_cast<size_t>(y) * columns + static_cast<size_t>(x);
                        state.cells[cell / 64] |= 1ull << (cell % 64);
                    }
                }
            }
            const uint32_t slot = candidate.slot;
            state.placed[slot] = free ? 1 : 0;
            if (first)
                state.alpha[slot] = free ? 1.0f : 0.0f;
            else
                state.alpha[slot] = free ? std::min(state.alpha[slot] + step, 1.0f) : std::max(state.alpha[slot] - step, 0.0f);
        }
    }
}
