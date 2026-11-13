// clang-format off
#pragma once

#include <cstdint>
#include <vector>

namespace rasm::rhi
{
    struct AliasRequest
    {
        uint64_t            size                = 0;
        uint64_t            alignment           = 1;
        uint32_t            memoryTypeBits      = 0;
        uint32_t            firstPass           = 0;
        uint32_t            lastPass            = 0;
    };

    struct AliasPlacement
    {
        uint32_t            heap                = 0;
        uint64_t            offset              = 0;
    };

    struct AliasHeap
    {
        uint64_t            size                = 0;
        uint64_t            alignment           = 1;
        uint32_t            memoryTypeBits      = 0;
    };

    void                    packAliased(const AliasRequest* requests, uint32_t count, AliasPlacement* placements, std::vector<AliasHeap>& heaps);
}
// clang-format on
