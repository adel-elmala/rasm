#include "rhi/uploadManager.h"

#include "spdlog/spdlog.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <numeric>
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

    bool UploadManager::initialize(Device &targetDevice)
    {
        device = &targetDevice;
        staging = device->createBuffer({.name = "Upload Staging Ring", .usage = BufferUsage::DEVICE_ADDRESS, .size = RING_SIZE, .hostVisible = true});
        fence = device->createFence(0, "Upload Fence");
        if (!staging || !fence)
        {
            spdlog::error("Failed to create the upload staging ring.");
            return false;
        }
        assert(staging->mapped);

        for (uint32_t i = 0; i < 2; ++i)
        {
            batches[i].commandBuffer = device->createCommandBuffer(QueueType::GRAPHICS, "Upload Command Buffer " + std::to_string(i));
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
            if (!allocate(chunk, 1, ringOffset) || !device->fillBuffer(staging, bytes, chunk, ringOffset))
                return false;

            addCopy({.dst = dst, .srcOffset = ringOffset, .dstOffset = offset, .size = chunk});
            bytes += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

    // Each mip goes in whole rows of blocks, as many as fit in a chunk, so a level bigger than a chunk (or than the
    // ring) is split across chunks and batches. The texture's first copy may drop its old contents; the later
    // ones keep what the earlier batches wrote.
    bool UploadManager::uploadTexture(Texture *dst, const TextureLevel *levels, uint32_t levelCount)
    {
        // The copies of one batch have no barriers between them, so filling a texture twice needs a new batch.
        const bool pending = std::any_of(pendingTextureCopies.begin(), pendingTextureCopies.end(), [dst](const TextureCopy &copy)
                                         { return copy.dst == dst; });
        if (pending && !flush())
            return false;

        const FormatBlock block = getFormatBlock(dst->desc.format);
        // A copy into an image starts at a multiple of the block size; 16 as well keeps the rows of every format
        // aligned for the copy engine. lcm(16, bytes): 16 for 4, 8 and 16 bytes, 48 for 3 and 12.
        const uint64_t alignment = std::lcm<uint64_t>(16, block.bytes);
        bool first = true;
        for (uint32_t mip = 0; mip < levelCount; ++mip)
        {
            const uint32_t width = std::max(1u, dst->desc.width >> mip);
            const uint32_t height = std::max(1u, dst->desc.height >> mip);
            const uint64_t rowBytes = static_cast<uint64_t>((width + block.width - 1) / block.width) * block.bytes;
            const uint32_t blockRows = (height + block.height - 1) / block.height;
            assert(rowBytes <= MAX_CHUNK_SIZE && levels[mip].size == rowBytes * blockRows);
            const uint32_t rowsPerChunk = static_cast<uint32_t>(std::min<uint64_t>(MAX_CHUNK_SIZE / rowBytes, blockRows));

            const auto *bytes = static_cast<const uint8_t *>(levels[mip].data);
            for (uint32_t row = 0; row < blockRows; row += rowsPerChunk)
            {
                const uint32_t rows = std::min(rowsPerChunk, blockRows - row);
                const uint64_t size = rows * rowBytes;
                uint64_t ringOffset = 0;
                if (!allocate(size, alignment, ringOffset) || !device->fillBuffer(staging, bytes + row * rowBytes, size, ringOffset))
                    return false;
                const uint32_t y = row * block.height;
                pendingTextureCopies.push_back({.dst = dst, .srcOffset = ringOffset, .mip = mip, .y = y, .height = std::min(rows * block.height, height - y), .discard = first});
                first = false;
            }
        }
        return true;
    }

    bool UploadManager::flush()
    {
        if (pendingCopies.empty() && pendingTextureCopies.empty())
            return true;

        ZoneScoped;

        // begin() resets the command buffer, so the GPU must be done with its previous submit.
        auto &batch = batches[currentBatch];
        bool recorded = device->waitForFence(fence, batch.fenceValue, UINT64_MAX) && batch.commandBuffer->begin();
        if (recorded)
        {
            if (!pendingCopies.empty())
                batch.commandBuffer->copyBuffers(staging, pendingCopies.data(), static_cast<uint32_t>(pendingCopies.size()));
            if (!pendingTextureCopies.empty())
                batch.commandBuffer->copyToTextures(staging, pendingTextureCopies.data(), static_cast<uint32_t>(pendingTextureCopies.size()));
            recorded = batch.commandBuffer->end();
        }

        // Same queue as the frames, submitted before the frame that uses the data: the barriers in
        // copyBuffers order it, and the fence only tells the CPU when the ring space is free again. Work on
        // another queue is not in those barriers: the copies wait for its last submit (waitFor), and that
        // queue's submits wait for the fence (lastSubmit). This holds for the flushes made while recording too.
        bool submitted = false;
        if (recorded)
        {
            CommandBuffer *commandBuffers[] = {batch.commandBuffer};
            const uint32_t waitCount = queueWait.fence != nullptr ? 1 : 0;
            submitted = device->getQueue(QueueType::GRAPHICS)->submit({.commandBuffers = commandBuffers, .commandBufferCount = 1, .waits = &queueWait, .waitCount = waitCount, .signalFence = fence, .signalValue = submittedValue + 1});
        }

        // A failed batch stays pending and goes out with the next flush: the caller was already told its
        // data is on the way. Its staging bytes are safe, since only a completed batch frees ring space.
        if (!submitted)
        {
            spdlog::error("Failed to submit the pending buffer uploads; the next flush retries them.");
            return false;
        }

        batch.fenceValue = ++submittedValue;
        batch.ringEnd = head;
        batch.buffers.clear();
        for (const auto &range : pendingRanges)
            batch.buffers.push_back(range.dst);
        batch.textures.clear();
        for (const auto &copy : pendingTextureCopies)
        {
            if (batch.textures.empty() || batch.textures.back() != copy.dst)
                batch.textures.push_back(copy.dst);
        }
        pendingCopies.clear();
        pendingRanges.clear();
        pendingTextureCopies.clear();
        currentBatch = (currentBatch + 1) % 2;
        return true;
    }

    FenceWait UploadManager::lastSubmit() const
    {
        return {.fence = fence, .value = submittedValue};
    }

    // The wait must be on a submit that already went out, or the copies wait for a value nothing signals.
    void UploadManager::waitFor(const FenceWait &wait)
    {
        queueWait = wait;
    }

    // The buffer is about to be destroyed: its pending copies must not run, and submitted ones must finish.
    // Only the two batches can still be in flight, since flush waits for a batch before reusing it.
    void UploadManager::cancelUploads(const Buffer *buffer)
    {
        std::erase_if(pendingCopies, [buffer](const BufferCopy &copy)
                      { return copy.dst == buffer; });
        std::erase_if(pendingRanges, [buffer](const WrittenRange &range)
                      { return range.dst == buffer; });

        for (const auto &batch : batches)
        {
            if (std::find(batch.buffers.begin(), batch.buffers.end(), buffer) != batch.buffers.end() &&
                !device->waitForFence(fence, batch.fenceValue, UINT64_MAX))
                spdlog::error("Failed to wait for the uploads to a buffer that is being destroyed.");
        }
    }

    void UploadManager::cancelUploads(const Texture *texture)
    {
        std::erase_if(pendingTextureCopies, [texture](const TextureCopy &copy)
                      { return copy.dst == texture; });

        for (const auto &batch : batches)
        {
            if (std::find(batch.textures.begin(), batch.textures.end(), texture) != batch.textures.end() &&
                !device->waitForFence(fence, batch.fenceValue, UINT64_MAX))
                spdlog::error("Failed to wait for the uploads to a texture that is being destroyed.");
        }
    }

    // head and tail count bytes since the start and never wrap: head - tail bytes are in use, and
    // head % RING_SIZE is where the next chunk goes. A chunk that would cross the end starts again at offset 0,
    // which is aligned for any alignment.
    bool UploadManager::allocate(uint64_t size, uint64_t alignment, uint64_t &ringOffset)
    {
        assert(size <= MAX_CHUNK_SIZE);

        const uint64_t position = head % RING_SIZE;
        const uint64_t alignPadding = (alignment - position % alignment) % alignment;
        const uint64_t padding = position + alignPadding + size > RING_SIZE ? RING_SIZE - position : alignPadding;

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
