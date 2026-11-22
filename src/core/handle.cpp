#include "rasm/handle.h"

namespace rasm
{
    HandlePool::HandlePool(uint64_t limit) : indexLimit(limit) {}

    HandleSlot HandlePool::allocate()
    {
        uint64_t index = 0;
        if (!freeIndices.empty())
        {
            index = freeIndices.back();
            freeIndices.pop_back();
        }
        else if (generations.size() < indexLimit)
        {
            index = generations.size();
            generations.push_back(1);
            alive.push_back(0);
        }
        else
            return {};

        alive[index] = 1;
        ++live;
        return {.index = index, .generation = generations[index]};
    }

    bool HandlePool::release(uint64_t index, uint64_t generation)
    {
        if (!isAlive(index, generation))
            return false;
        alive[index] = 0;
        --live;
        if (generations[index] < UINT64_MAX)
        {
            ++generations[index];
            freeIndices.push_back(index);
        }
        return true;
    }

    bool HandlePool::isAlive(uint64_t index, uint64_t generation) const
    {
        return generation != 0 && index < generations.size() && alive[index] != 0 && generations[index] == generation;
    }

    uint64_t HandlePool::size() const
    {
        return generations.size();
    }

    uint64_t HandlePool::liveCount() const
    {
        return live;
    }
} // namespace rasm
