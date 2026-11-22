// clang-format off
#pragma once

#include <cstdint>

struct AllocationCount
{
    uint64_t    count       = 0;
    uint64_t    bytes       = 0;
    uint64_t    firstSize   = 0;
};

bool            allocationCountingWorks();
void            startCountingAllocations();
AllocationCount stopCountingAllocations();
// clang-format on
