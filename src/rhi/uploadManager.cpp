#include "rhi/uploadManager.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <string>

namespace rasm::rhi
{
    namespace
    {
        constexpr uint64_t RING_SIZE = 64ull * 1024 * 1024;
        // A chunk never wraps, so it may need padding to the end of the ring first. A quarter of the ring
        // plus that padding still fits in an empty ring.
        constexpr uint64_t MAX_CHUNK_SIZE = RING_SIZE / 4;
    }

    bool UploadManager::initialize(Device &device)
    {
        this->device = &device;
        staging = device.createBuffer({.name = "Upload Staging Ring", .usage = BufferUsage::DEVICE_ADDRESS, .size = RING_SIZE, .hostVisible = true});
        fence = device.createFence(0, "Upload Fence");
        if (!staging || !fence)
        {
            spdlog::error("Failed to create the upload staging ring.");
            return false;
        }
        assert(staging->mapped);

        for (uint32_t i = 0; i < 2; ++i)
        {
            batches[i].commandBuffer = device.createCommandBuffer(QueueType::GRAPHICS, "Upload Command Buffer " + std::to_string(i));
            if (!batches[i].commandBuffer)
                return false;
        }
        return true;
    }

    // The device must be idle.
    void UploadManager::cleanup()
    {
        if (!device)
            return;

        for (auto &batch : batches)
        {
            if (batch.commandBuffer)
                device->destroyCommandBuffer(batch.commandBuffer);
        }
        if (fence)
            device->destroyFence(fence);
        if (staging)
            device->destroyBuffer(staging);
        *this = UploadManager{};
    }

    bool UploadManager::upload(Buffer *dst, const void *data, size_t size, size_t offset)
    {
        assert(offset + size <= dst->desc.size);

        // Copies in one batch have no barriers between them, so writing a range twice needs a new batch.
        // The check uses the span of everything written to dst in this batch: exact for the usual ascending fills,
        // and only a spare flush otherwise.
        for (const auto &range : pendingRanges)
        {
            if (range.dst == dst && offset < range.end && range.begin < offset + size)
            {
                if (!flush())
                    return false;
                break;
            }
        }

        const auto *bytes = static_cast<const uint8_t *>(data);
        while (size > 0)
        {
            const uint64_t chunk = std::min<uint64_t>(size, MAX_CHUNK_SIZE);
            uint64_t ringOffset = 0;
            if (!allocate(chunk, ringOffset) || !device->fillBuffer(staging, bytes, chunk, ringOffset))
                return false;

            addCopy({.dst = dst, .srcOffset = ringOffset, .dstOffset = offset, .size = chunk});
            bytes += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

    bool UploadManager::flush()
    {
        if (pendingCopies.empty())
            return true;

        ZoneScoped;

        // begin() resets the command buffer, so the GPU must be done with its previous submit.
        auto &batch = batches[currentBatch];
        bool recorded = device->waitForFence(fence, batch.fenceValue, UINT64_MAX) && batch.commandBuffer->begin();
        if (recorded)
        {
            batch.commandBuffer->copyBuffers(staging, pendingCopies.data(), static_cast<uint32_t>(pendingCopies.size()));
            recorded = batch.commandBuffer->end();
        }

        // Same queue as the frames, submitted before the frame that uses the data: the barriers in
        // copyBuffers order it, and the fence only tells the CPU when the ring space is free again.
        bool submitted = false;
        if (recorded)
        {
            CommandBuffer *commandBuffers[] = {batch.commandBuffer};
            submitted = device->getQueue(QueueType::GRAPHICS)->submit({.commandBuffers = commandBuffers, .commandBufferCount = 1, .signalFence = fence, .signalValue = submittedValue + 1});
        }

        // A failed batch is dropped. Its ring space is freed with the next batch that completes.
        pendingCopies.clear();
        pendingRanges.clear();
        if (!submitted)
        {
            spdlog::error("Failed to submit the pending buffer uploads.");
            return false;
        }

        batch.fenceValue = ++submittedValue;
        batch.ringEnd = head;
        currentBatch = (currentBatch + 1) % 2;
        return true;
    }

    // The buffer is about to be destroyed: its copies must not run. Submitted copies are covered by the
    // caller's promise that the GPU no longer uses the buffer.
    void UploadManager::cancelUploads(const Buffer *buffer)
    {
        std::erase_if(pendingCopies, [buffer](const BufferCopy &copy)
                      { return copy.dst == buffer; });
        std::erase_if(pendingRanges, [buffer](const WrittenRange &range)
                      { return range.dst == buffer; });
    }

    // head and tail count bytes since the start and never wrap: head - tail bytes are in use, and
    // head % RING_SIZE is where the next chunk goes.
    bool UploadManager::allocate(uint64_t size, uint64_t &ringOffset)
    {
        assert(size <= MAX_CHUNK_SIZE);

        const uint64_t position = head % RING_SIZE;
        const uint64_t padding = position + size > RING_SIZE ? RING_SIZE - position : 0;

        reclaim();
        if (head + padding + size - tail > RING_SIZE)
        {
            // Full: submit what is pending and wait for every copy, which frees the whole ring.
            if (!flush() || !device->waitForFence(fence, submittedValue, UINT64_MAX))
                return false;
            tail = head;
        }

        head += padding;
        ringOffset = head % RING_SIZE;
        head += size;
        return true;
    }

    void UploadManager::addCopy(const BufferCopy &copy)
    {
        auto *last = pendingCopies.empty() ? nullptr : &pendingCopies.back();
        if (last && last->dst == copy.dst && last->srcOffset + last->size == copy.srcOffset && last->dstOffset + last->size == copy.dstOffset)
            last->size += copy.size;
        else
            pendingCopies.push_back(copy);

        for (auto &range : pendingRanges)
        {
            if (range.dst == copy.dst)
            {
                range.begin = std::min(range.begin, copy.dstOffset);
                range.end = std::max(range.end, copy.dstOffset + copy.size);
                return;
            }
        }
        pendingRanges.push_back({.dst = copy.dst, .begin = copy.dstOffset, .end = copy.dstOffset + copy.size});
    }

    void UploadManager::reclaim()
    {
        const uint64_t completed = device->getFenceValue(fence);
        for (const auto &batch : batches)
        {
            if (batch.fenceValue != 0 && batch.fenceValue <= completed)
                tail = std::max(tail, batch.ringEnd);
        }
    }
}
