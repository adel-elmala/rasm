#include "rhi/uploadManager.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace rasm;
using namespace rasm::rhi;

namespace
{
    constexpr uint64_t MB = 1024ull * 1024;
    constexpr uint64_t RING_SIZE = 64 * MB; // matches uploadManager.cpp

    struct FakeBuffer : Buffer
    {
        std::vector<uint8_t> bytes;
    };

    struct FakeFence : Fence
    {
        uint64_t completed = 0;
    };

    class FakeCommandBuffer : public CommandBuffer
    {
    public:
        bool begin() override
        {
            if (inFlight)
                beganWhileInFlight = true;
            src = nullptr;
            copies.clear();
            return true;
        }
        bool end() override { return true; }
        bool transitionTexture(Texture *, TextureUsage, TextureUsage) override { return true; }
        void beginRendering(Texture *, Texture *, const glm::vec4 &) override {}
        void endRendering() override {}
        void setViewport(float, float, float, float, float, float) override {}
        void setScissor(int32_t, int32_t, uint32_t, uint32_t) override {}
        void bindPipeline(Pipeline *) override {}
        void bindIndexBuffer(Buffer *, uint64_t, Format) override {}
        void bindDescriptorSet(Pipeline *, DescriptorSet *, uint32_t) override {}
        void pushConstants(Pipeline *, ShaderType, const void *, uint32_t, uint32_t) override {}
        void draw(uint32_t, uint32_t) override {}
        void drawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void drawIndexedIndirect(Buffer *, uint64_t, uint32_t, uint32_t) override {}
        void copyBuffers(Buffer *source, const BufferCopy *list, uint32_t count) override
        {
            src = source;
            copies.insert(copies.end(), list, list + count);
        }

        Buffer *src = nullptr;
        std::vector<BufferCopy> copies;
        bool inFlight = false;
        bool beganWhileInFlight = false;
    };

    struct Submission
    {
        FakeCommandBuffer *commandBuffer = nullptr;
        Buffer *src = nullptr;
        std::vector<BufferCopy> copies;
        FakeFence *fence = nullptr;
        uint64_t signalValue = 0;
    };

    class FakeQueue : public Queue
    {
    public:
        bool submit(const SubmitDesc &desc) override
        {
            if (failNextSubmit)
            {
                failNextSubmit = false;
                return false;
            }
            for (uint32_t i = 0; i < desc.commandBufferCount; ++i)
            {
                auto *commandBuffer = static_cast<FakeCommandBuffer *>(desc.commandBuffers[i]);
                commandBuffer->inFlight = true;
                submissions.push_back({.commandBuffer = commandBuffer, .src = commandBuffer->src, .copies = commandBuffer->copies, .fence = static_cast<FakeFence *>(desc.signalFence), .signalValue = desc.signalValue});
            }
            return true;
        }

        std::vector<Submission> submissions;
        size_t executed = 0;
        bool failNextSubmit = false;
    };

    // The "GPU" only runs submitted copies when a test or a blocking wait lets it, and it reads the staging
    // bytes at that moment. So a ring slot overwritten before its copy ran shows up as wrong data.
    class FakeDevice : public Device
    {
    public:
        ~FakeDevice() override
        {
            for (auto *buffer : buffers)
                delete buffer;
            for (auto *commandBuffer : commandBuffers)
                delete commandBuffer;
            for (auto *fence : fences)
                delete fence;
        }

        Buffer *createBuffer(const BufferDesc &desc) override
        {
            auto *buffer = new FakeBuffer();
            buffer->desc = desc;
            buffer->mapped = desc.hostVisible;
            buffer->bytes.resize(desc.size);
            buffers.push_back(buffer);
            return buffer;
        }
        Texture *createTexture(const TextureDesc &) override { return nullptr; }
        Sampler *createSampler(const SamplerDesc &) override { return nullptr; }
        Shader *createShader(const ShaderDesc &) override { return nullptr; }
        Pipeline *createGraphicsPipeline(const PipelineDesc &, Shader *, Shader *, DescriptorSetLayout *) override { return nullptr; }
        DescriptorSetLayout *createBindlessDescriptorSetLayout(const BindlessDescriptorSetLayoutDesc &) override { return nullptr; }
        DescriptorPool *createDescriptorPool(const DescriptorPoolDesc &) override { return nullptr; }
        DescriptorSet *allocateDescriptorSet(const std::string &, DescriptorPool *, DescriptorSetLayout *) override { return nullptr; }
        CommandBuffer *createCommandBuffer(QueueType, const std::string &) override
        {
            auto *commandBuffer = new FakeCommandBuffer();
            commandBuffers.push_back(commandBuffer);
            return commandBuffer;
        }
        Fence *createFence(uint64_t initialValue, const std::string &) override
        {
            auto *fence = new FakeFence();
            fence->completed = initialValue;
            fences.push_back(fence);
            return fence;
        }
        rhi::Swapchain *createSwapchain(const SwapchainDesc &) override { return nullptr; }
        void destroyBuffer(Buffer *buffer) override
        {
            std::erase(buffers, buffer);
            delete static_cast<FakeBuffer *>(buffer);
        }
        void destroyTexture(Texture *) override {}
        void destroySampler(Sampler *) override {}
        void destroyShader(Shader *) override {}
        void destroyPipeline(Pipeline *) override {}
        void destroyDescriptorSetLayout(DescriptorSetLayout *) override {}
        void destroyDescriptorPool(DescriptorPool *) override {}
        void destroyDescriptorSet(DescriptorSet *) override {}
        void destroyCommandBuffer(CommandBuffer *commandBuffer) override
        {
            std::erase(commandBuffers, commandBuffer);
            delete static_cast<FakeCommandBuffer *>(commandBuffer);
        }
        void destroyFence(Fence *fence) override
        {
            std::erase(fences, fence);
            delete static_cast<FakeFence *>(fence);
        }
        void destroySwapchain(rhi::Swapchain *) override {}
        Queue *getQueue(QueueType) override { return &queue; }
        bool fillBuffer(Buffer *buffer, const void *data, size_t size, size_t offset) override
        {
            auto *fake = static_cast<FakeBuffer *>(buffer);
            if (!fake->mapped || offset + size > fake->bytes.size())
            {
                ++badFills;
                return false;
            }
            std::memcpy(fake->bytes.data() + offset, data, size);
            return true;
        }
        bool fillTexture(Texture *, const void *) override { return false; }
        void updateBindlessTexture(DescriptorSet *, uint32_t, Texture *, Sampler *) override {}
        bool waitForFence(Fence *fence, uint64_t value, uint64_t) override
        {
            auto *fake = static_cast<FakeFence *>(fence);
            if (fake->completed < value)
            {
                ++blockingWaits;
                runGpuUntil(value);
            }
            return fake->completed >= value;
        }
        uint64_t getFenceValue(Fence *fence) override { return static_cast<FakeFence *>(fence)->completed; }
        VRAMStats queryVRAMStats() override { return {}; }
        void waitIdle() override { finishGpuWork(); }

        void runGpuUntil(uint64_t value)
        {
            auto &list = queue.submissions;
            while (queue.executed < list.size() && list[queue.executed].signalValue <= value)
            {
                auto &submission = list[queue.executed++];
                for (const auto &copy : submission.copies)
                    execute(submission.src, copy);
                submission.commandBuffer->inFlight = false;
                if (submission.fence)
                    submission.fence->completed = submission.signalValue;
            }
        }

        void finishGpuWork() { runGpuUntil(UINT64_MAX); }

        bool isLive(const Buffer *buffer) const { return std::find(buffers.begin(), buffers.end(), buffer) != buffers.end(); }

        void execute(Buffer *source, const BufferCopy &copy)
        {
            if (!isLive(source) || !isLive(copy.dst))
            {
                ++badCopies;
                return;
            }
            auto *src = static_cast<FakeBuffer *>(source);
            auto *dst = static_cast<FakeBuffer *>(copy.dst);
            if (copy.srcOffset + copy.size > src->bytes.size() || copy.dstOffset + copy.size > dst->bytes.size())
            {
                ++badCopies;
                return;
            }
            std::memcpy(dst->bytes.data() + copy.dstOffset, src->bytes.data() + copy.srcOffset, copy.size);
        }

        FakeQueue queue;
        std::vector<FakeBuffer *> buffers;
        std::vector<FakeCommandBuffer *> commandBuffers;
        std::vector<FakeFence *> fences;
        int blockingWaits = 0;
        int badCopies = 0;
        int badFills = 0;
    };

    // Bytes that depend on every bit of the index, so data copied from a wrong offset never matches.
    std::vector<uint8_t> makeBytes(uint64_t size, uint32_t seed)
    {
        std::vector<uint8_t> bytes(size);
        for (uint64_t i = 0; i < size; ++i)
        {
            uint32_t x = static_cast<uint32_t>(i) * 2654435761u + seed * 0x9E3779B9u;
            x ^= x >> 15;
            bytes[i] = static_cast<uint8_t>(x >> 24);
        }
        return bytes;
    }

    bool holds(const FakeBuffer *buffer, uint64_t offset, const std::vector<uint8_t> &expected)
    {
        return offset + expected.size() <= buffer->bytes.size() && std::memcmp(buffer->bytes.data() + offset, expected.data(), expected.size()) == 0;
    }

    struct UploadFixture
    {
        UploadFixture() { REQUIRE(uploads.initialize(device)); }

        ~UploadFixture()
        {
            device.finishGpuWork();
            uploads.cleanup();
            CHECK(device.badCopies == 0);
            CHECK(device.badFills == 0);
            for (auto *commandBuffer : device.commandBuffers)
                CHECK_FALSE(commandBuffer->beganWhileInFlight);
        }

        FakeBuffer *createDeviceLocal(uint64_t size)
        {
            return static_cast<FakeBuffer *>(device.createBuffer({.name = "dst", .usage = BufferUsage::STORAGE, .size = size}));
        }

        bool upload(FakeBuffer *dst, const std::vector<uint8_t> &data, uint64_t offset)
        {
            return uploads.upload(dst, data.data(), data.size(), offset);
        }

        FakeDevice device;
        UploadManager uploads;
    };
}

TEST_CASE("initialize creates a mapped staging ring, two command buffers and a fence, and cleanup destroys them")
{
    FakeDevice device;
    UploadManager uploads;
    REQUIRE(uploads.initialize(device));

    REQUIRE(device.buffers.size() == 1);
    CHECK(device.buffers[0]->mapped);
    CHECK(device.buffers[0]->desc.size == RING_SIZE);
    CHECK(device.commandBuffers.size() == 2);
    CHECK(device.fences.size() == 1);

    uploads.cleanup();

    CHECK(device.buffers.empty());
    CHECK(device.commandBuffers.empty());
    CHECK(device.fences.empty());
}

TEST_CASE_FIXTURE(UploadFixture, "an upload reaches the buffer only after flush and the GPU copy")
{
    auto *dst = createDeviceLocal(256);
    const auto data = makeBytes(256, 1);

    REQUIRE(upload(dst, data, 0));
    CHECK(device.queue.submissions.empty());

    REQUIRE(uploads.flush());
    REQUIRE(device.queue.submissions.size() == 1);
    CHECK_FALSE(holds(dst, 0, data));

    device.finishGpuWork();
    CHECK(holds(dst, 0, data));
}

TEST_CASE_FIXTURE(UploadFixture, "flush with nothing pending submits nothing")
{
    CHECK(uploads.flush());
    CHECK(device.queue.submissions.empty());
}

TEST_CASE_FIXTURE(UploadFixture, "an upload at an offset writes only that range")
{
    auto *dst = createDeviceLocal(300);
    std::fill(dst->bytes.begin(), dst->bytes.end(), uint8_t{0xEE});
    const auto data = makeBytes(100, 2);

    REQUIRE(upload(dst, data, 100));
    REQUIRE(uploads.flush());
    device.finishGpuWork();

    CHECK(holds(dst, 0, std::vector<uint8_t>(100, 0xEE)));
    CHECK(holds(dst, 100, data));
    CHECK(holds(dst, 200, std::vector<uint8_t>(100, 0xEE)));
}

TEST_CASE_FIXTURE(UploadFixture, "each submit signals the next fence value")
{
    auto *dst = createDeviceLocal(16);
    const auto data = makeBytes(16, 3);

    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(upload(dst, data, 0));
        REQUIRE(uploads.flush());
        device.finishGpuWork();
    }

    REQUIRE(device.queue.submissions.size() == 3);
    for (uint64_t i = 0; i < 3; ++i)
    {
        CHECK(device.queue.submissions[i].fence == device.fences[0]);
        CHECK(device.queue.submissions[i].signalValue == i + 1);
    }
}

TEST_CASE_FIXTURE(UploadFixture, "back-to-back uploads to one buffer merge into one copy")
{
    auto *dst = createDeviceLocal(200);
    const auto first = makeBytes(100, 4);
    const auto second = makeBytes(100, 5);

    REQUIRE(upload(dst, first, 0));
    REQUIRE(upload(dst, second, 100));
    REQUIRE(uploads.flush());

    REQUIRE(device.queue.submissions.size() == 1);
    REQUIRE(device.queue.submissions[0].copies.size() == 1);
    CHECK(device.queue.submissions[0].copies[0].size == 200);

    device.finishGpuWork();
    CHECK(holds(dst, 0, first));
    CHECK(holds(dst, 100, second));
}

TEST_CASE_FIXTURE(UploadFixture, "uploads to different buffers share a batch as separate copies")
{
    auto *a = createDeviceLocal(64);
    auto *b = createDeviceLocal(64);
    const auto dataA = makeBytes(64, 6);
    const auto dataB = makeBytes(64, 7);

    REQUIRE(upload(a, dataA, 0));
    REQUIRE(upload(b, dataB, 0));
    REQUIRE(uploads.flush());

    REQUIRE(device.queue.submissions.size() == 1);
    CHECK(device.queue.submissions[0].copies.size() == 2);

    device.finishGpuWork();
    CHECK(holds(a, 0, dataA));
    CHECK(holds(b, 0, dataB));
}

TEST_CASE_FIXTURE(UploadFixture, "writing a range twice in one batch starts a new batch, and the second write wins")
{
    auto *dst = createDeviceLocal(200);
    const auto first = makeBytes(100, 8);
    const auto second = makeBytes(100, 9);

    REQUIRE(upload(dst, first, 0));
    REQUIRE(upload(dst, second, 50));
    CHECK(device.queue.submissions.size() == 1);

    REQUIRE(uploads.flush());
    CHECK(device.queue.submissions.size() == 2);

    device.finishGpuWork();
    CHECK(holds(dst, 0, std::vector<uint8_t>(first.begin(), first.begin() + 50)));
    CHECK(holds(dst, 50, second));
}

TEST_CASE_FIXTURE(UploadFixture, "ranges that only touch or are apart do not start a new batch")
{
    auto *dst = createDeviceLocal(400);
    const auto a = makeBytes(100, 10);
    const auto b = makeBytes(100, 11);
    const auto c = makeBytes(100, 12);

    REQUIRE(upload(dst, a, 100));
    REQUIRE(upload(dst, b, 0));
    REQUIRE(upload(dst, c, 300));
    CHECK(device.queue.submissions.empty());

    REQUIRE(uploads.flush());
    CHECK(device.queue.submissions.size() == 1);

    device.finishGpuWork();
    CHECK(holds(dst, 0, b));
    CHECK(holds(dst, 100, a));
    CHECK(holds(dst, 300, c));
}

TEST_CASE_FIXTURE(UploadFixture, "an upload bigger than a chunk arrives intact")
{
    auto *dst = createDeviceLocal(40 * MB + 123);
    const auto data = makeBytes(40 * MB + 123, 13);

    REQUIRE(upload(dst, data, 0));
    REQUIRE(uploads.flush());
    device.finishGpuWork();

    CHECK(holds(dst, 0, data));
    CHECK(device.blockingWaits == 0);
}

TEST_CASE_FIXTURE(UploadFixture, "the ring wraps to the start instead of splitting a chunk at the end")
{
    auto *first = createDeviceLocal(50 * MB);
    auto *second = createDeviceLocal(20 * MB);
    const auto firstData = makeBytes(50 * MB, 14);
    const auto secondData = makeBytes(20 * MB, 15);

    REQUIRE(upload(first, firstData, 0));
    REQUIRE(uploads.flush());
    device.finishGpuWork();

    REQUIRE(upload(second, secondData, 0));
    REQUIRE(uploads.flush());

    REQUIRE(device.queue.submissions.size() == 2);
    REQUIRE(device.queue.submissions[1].copies.size() == 1);
    CHECK(device.queue.submissions[1].copies[0].srcOffset == 0);

    device.finishGpuWork();
    CHECK(holds(first, 0, firstData));
    CHECK(holds(second, 0, secondData));
    CHECK(device.blockingWaits == 0);
}

TEST_CASE_FIXTURE(UploadFixture, "a full ring waits for the GPU instead of overwriting data still in flight")
{
    auto *dst = createDeviceLocal(80 * MB);
    const auto data = makeBytes(80 * MB, 16);

    REQUIRE(upload(dst, data, 0));
    CHECK(device.blockingWaits == 1);

    REQUIRE(uploads.flush());
    device.finishGpuWork();
    CHECK(holds(dst, 0, data));
}

TEST_CASE_FIXTURE(UploadFixture, "a full ring flushes the copies of every pending buffer")
{
    constexpr uint32_t COUNT = 8;
    constexpr uint64_t SIZE = 10 * MB;
    std::vector<FakeBuffer *> buffers;
    for (uint32_t i = 0; i < COUNT; ++i)
    {
        buffers.push_back(createDeviceLocal(SIZE));
        REQUIRE(upload(buffers[i], makeBytes(SIZE, 100 + i), 0));
    }
    CHECK(device.blockingWaits == 1);

    REQUIRE(uploads.flush());
    device.finishGpuWork();
    for (uint32_t i = 0; i < COUNT; ++i)
        CHECK(holds(buffers[i], 0, makeBytes(SIZE, 100 + i)));
}

TEST_CASE_FIXTURE(UploadFixture, "ring space freed by finished batches is reused without waiting")
{
    auto *dst = createDeviceLocal(12 * MB);

    for (uint32_t round = 0; round < 8; ++round)
    {
        REQUIRE(upload(dst, makeBytes(12 * MB, 200 + round), 0));
        REQUIRE(uploads.flush());
        device.finishGpuWork();
    }

    CHECK(device.blockingWaits == 0);
    CHECK(holds(dst, 0, makeBytes(12 * MB, 207)));
}

TEST_CASE_FIXTURE(UploadFixture, "a command buffer is reused only after the GPU finished its last submit")
{
    auto *a = createDeviceLocal(32);
    auto *b = createDeviceLocal(32);
    auto *c = createDeviceLocal(32);
    const auto dataA = makeBytes(32, 17);
    const auto dataB = makeBytes(32, 18);
    const auto dataC = makeBytes(32, 19);

    REQUIRE(upload(a, dataA, 0));
    REQUIRE(uploads.flush());
    REQUIRE(upload(b, dataB, 0));
    REQUIRE(uploads.flush());
    CHECK(device.queue.executed == 0);
    CHECK(device.blockingWaits == 0);

    // Both command buffers are in flight, so the third batch waits for the first one only.
    REQUIRE(upload(c, dataC, 0));
    REQUIRE(uploads.flush());
    CHECK(device.blockingWaits == 1);
    CHECK(device.queue.executed == 1);

    device.finishGpuWork();
    CHECK(holds(a, 0, dataA));
    CHECK(holds(b, 0, dataB));
    CHECK(holds(c, 0, dataC));
}

TEST_CASE_FIXTURE(UploadFixture, "cancelled uploads never run, so the buffer can be destroyed")
{
    auto *a = createDeviceLocal(64);
    auto *b = createDeviceLocal(64);
    const auto dataB = makeBytes(64, 20);

    REQUIRE(upload(a, makeBytes(64, 21), 0));
    REQUIRE(upload(b, dataB, 0));
    uploads.cancelUploads(a);
    device.destroyBuffer(a);

    REQUIRE(uploads.flush());
    REQUIRE(device.queue.submissions.size() == 1);
    CHECK(device.queue.submissions[0].copies.size() == 1);

    device.finishGpuWork();
    CHECK(holds(b, 0, dataB));
}

TEST_CASE_FIXTURE(UploadFixture, "cancelling forgets the written range, so writing it again needs no new batch")
{
    auto *dst = createDeviceLocal(64);
    const auto data = makeBytes(64, 22);

    REQUIRE(upload(dst, makeBytes(64, 23), 0));
    uploads.cancelUploads(dst);
    REQUIRE(upload(dst, data, 0));
    CHECK(device.queue.submissions.empty());

    REQUIRE(uploads.flush());
    device.finishGpuWork();
    CHECK(holds(dst, 0, data));
}

TEST_CASE_FIXTURE(UploadFixture, "a failed submit drops its batch and later uploads still work")
{
    auto *a = createDeviceLocal(64);
    auto *b = createDeviceLocal(64);
    const auto dataB = makeBytes(64, 24);

    REQUIRE(upload(a, makeBytes(64, 25), 0));
    device.queue.failNextSubmit = true;
    CHECK_FALSE(uploads.flush());
    CHECK(device.queue.submissions.empty());
    CHECK(uploads.flush());

    REQUIRE(upload(b, dataB, 0));
    REQUIRE(uploads.flush());
    device.finishGpuWork();

    CHECK(holds(a, 0, std::vector<uint8_t>(64, 0)));
    CHECK(holds(b, 0, dataB));
}
