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
        bool                        initialize(Device& targetDevice, QueueType queueType);
        void                        cleanup();
        bool                        upload(Buffer* dst, const void* data, size_t size, size_t offset);
        bool                        uploadTexture(Texture* dst, const TextureLevel* levels, uint32_t levelCount);
        bool                        uploadTexture(Texture* dst, const TextureLevel* levels, uint32_t levelCount, TextureFillProgress& progress, uint64_t maxBytes);
        bool                        flush();
        FenceWait                   lastSubmit() const;
        uint64_t                    coveredValue() const;
        void                        waitFor(const FenceWait* waits, uint32_t count);
        bool                        isFilling(const Texture* texture);
        CommandBuffer*              recordAcquires(uint64_t completed);
        void                        acquiresSubmitted(Fence* fence, uint64_t value);
        uint64_t                    takeGpuNanoseconds();
        void                        cancelUploads(const Buffer* buffer);
        void                        cancelUploads(const Texture* texture);

    private:
        struct Batch
        {
            CommandBuffer*                  commandBuffer       = nullptr;
            QueryPool*                      timer               = nullptr;  // its start and end, with timestamps on the queue
            bool                            timed               = false;    // the timer holds a submit's times, not read yet
            uint64_t                        fenceValue          = 0;
            uint64_t                        ringEnd             = 0;
            std::vector<const Buffer*>      buffers;            // written by the last submit, for cancelUploads
            std::vector<const Texture*>     textures;
        };

        struct WrittenRange
        {
            const Buffer*           dst                 = nullptr;
            uint64_t                begin               = 0;
            uint64_t                end                 = 0;
        };

        // A texture a submit on the transfer queue released to the graphics family, and the value it signals.
        struct Release
        {
            Texture*                texture             = nullptr;
            uint64_t                value               = 0;
        };

        // Graphics command buffers for the acquires, each reused once the submit that took it is done.
        struct AcquireBatch
        {
            CommandBuffer*          commandBuffer       = nullptr;
            FenceWait               submit              = {};
        };

        static constexpr uint32_t   ACQUIRE_BATCHES     = 3;    // the frames in flight, and one being recorded

        bool                        allocate(uint64_t size, uint64_t alignment, uint64_t& ringOffset);
        bool                        hasRoom(uint64_t size, uint64_t alignment);
        void                        addCopy(const BufferCopy& copy);
        void                        reclaim();

        Device*                     device              = nullptr;
        QueueType                   queue               = QueueType::GRAPHICS;
        Buffer*                     staging             = nullptr;
        Fence*                      fence               = nullptr;
        Batch                       batches[2]          = {};
        uint32_t                    currentBatch        = 0;
        uint64_t                    submittedValue      = 0;
        FenceWait                   queueWaits[2]       = {};   // what every flush waits for: other queues' submits
        uint32_t                    queueWaitCount      = 0;
        uint64_t                    head                = 0;
        uint64_t                    tail                = 0;
        uint64_t                    gpuNanoseconds      = 0;    // of the batches done since takeGpuNanoseconds
        std::vector<BufferCopy>     pendingCopies;
        std::vector<WrittenRange>   pendingRanges;
        std::vector<TextureCopy>    pendingTextureCopies;
        std::vector<Release>        releases;
        std::vector<Texture*>       acquiring;          // in the acquire command buffer recordAcquires gave out
        AcquireBatch                acquireBatches[ACQUIRE_BATCHES] = {};
        uint32_t                    currentAcquire      = 0;
    };
}
// clang-format on
