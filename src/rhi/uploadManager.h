// clang-format off
#pragma once

#include "rhi/device.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rasm::rhi
{
    class UploadManager
    {
    public:
        bool                        initialize(Device& device);
        void                        cleanup();
        bool                        upload(Buffer* dst, const void* data, size_t size, size_t offset);
        bool                        flush();
        void                        cancelUploads(const Buffer* buffer);

    private:
        struct Batch
        {
            CommandBuffer*          commandBuffer       = nullptr;
            uint64_t                fenceValue          = 0;
            uint64_t                ringEnd             = 0;
        };

        struct WrittenRange
        {
            const Buffer*           dst                 = nullptr;
            uint64_t                begin               = 0;
            uint64_t                end                 = 0;
        };

        bool                        allocate(uint64_t size, uint64_t& ringOffset);
        void                        addCopy(const BufferCopy& copy);
        void                        reclaim();

        Device*                     device              = nullptr;
        Buffer*                     staging             = nullptr;
        Fence*                      fence               = nullptr;
        Batch                       batches[2]          = {};
        uint32_t                    currentBatch        = 0;
        uint64_t                    submittedValue      = 0;
        uint64_t                    head                = 0;
        uint64_t                    tail                = 0;
        std::vector<BufferCopy>     pendingCopies;
        std::vector<WrittenRange>   pendingRanges;
    };
}
// clang-format on
