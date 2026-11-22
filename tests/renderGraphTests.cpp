#include "rasm/render/renderGraph.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    // compile() needs no engine, so a graph without one can be sorted but not executed.
    class TestGraph : public rasm::RenderGraph
    {
    public:
        TestGraph() : RenderGraph(nullptr) {}

        // A graphics pass must draw to one target, so each gets its own and the textures alone decide the order.
        rasm::RenderTargetHandle ownTarget() const { return {1000 + passes.size(), 1}; }

        void addTexturePass(const std::vector<rasm::TextureHandle> &reads, const std::vector<rasm::TextureHandle> &writes)
        {
            addPass(
                "pass",
                [&](rasm::PassBuilder &builder)
                {
                    for (auto handle : reads)
                    {
                        builder.read(handle);
                    }
                    for (auto handle : writes)
                    {
                        builder.write(handle);
                    }
                    builder.write(ownTarget());
                },
                [](rasm::RenderContext &) {});
        }

        void addBufferPass(bool compute, const std::vector<rasm::BufferHandle> &reads, const std::vector<rasm::BufferHandle> &writes)
        {
            const auto setup = [&](rasm::PassBuilder &builder)
            {
                for (auto handle : reads)
                {
                    builder.read(handle);
                }
                for (auto handle : writes)
                {
                    builder.write(handle);
                }
                if (!compute)
                    builder.write(ownTarget());
            };
            if (compute)
                addComputePass("compute", setup, [](rasm::RenderContext &) {});
            else
                addPass("graphics", setup, [](rasm::RenderContext &) {});
        }

        // A compute pass writes the buffers, and a graphics pass reads them.
        void addFrameBufferPass(bool async, const rasm::BufferHandle (&perFrame)[rasm::MAX_FRAMES_IN_FLIGHT])
        {
            if (async)
                addAsyncComputePass("cull", [&](rasm::PassBuilder &builder)
                                    { builder.write(perFrame); }, [](rasm::RenderContext &) {});
            else
            {
                addPass(
                    "draw",
                    [&](rasm::PassBuilder &builder)
                    {
                        builder.read(perFrame);
                        builder.write(ownTarget());
                    },
                    [](rasm::RenderContext &) {});
            }
        }

        // What execute records before the pass: false for no barrier.
        bool barrierBefore(uint32_t passIdx, rasm::PipelineStage &before, rasm::PipelineStage &after) { return bufferBarrier(passes[passIdx], rasm::QueueType::GRAPHICS, before, after); }

        // createRenderTarget needs an engine, so a test marks a handle transient directly.
        void addTransient(rasm::RenderTargetHandle renderTarget) { transientTargets.push_back(renderTarget); }
        uint32_t firstPass(uint32_t transient) const { return transientFirstPasses[transient]; }
        uint32_t lastPass(uint32_t transient) const { return transientLastPasses[transient]; }

        // Engine::updateInspectPass's pass: a compute pass that copies what it reads.
        void addInspectPass(rasm::TextureHandle source, rasm::TextureHandle copy)
        {
            addComputePass(
                "Inspect",
                [&](rasm::PassBuilder &builder)
                {
                    builder.read(source);
                    builder.write(copy);
                },
                [](rasm::RenderContext &) {});
            inspectPass = static_cast<uint32_t>(passes.size() - 1);
        }
        void remove(uint32_t passIdx) { removePass(passIdx); }
        uint32_t inspect() const { return inspectPass; }
        size_t passCount() const { return passes.size(); }

        const std::vector<uint32_t> &order() const { return executionOrder; }
        const rasm::Pass &pass(uint32_t passIdx) const { return passes[passIdx]; }
        bool isCompiled() const { return compiled; }
    };

    size_t positionOf(const TestGraph &graph, uint32_t passIdx)
    {
        const auto &order = graph.order();
        return std::find(order.begin(), order.end(), passIdx) - order.begin();
    }

    constexpr rasm::TextureHandle TEXTURE_A{1, 1};
    constexpr rasm::TextureHandle TEXTURE_B{2, 1};
    constexpr rasm::TextureHandle TEXTURE_C{3, 1};
}

TEST_CASE("an empty graph compiles")
{
    TestGraph graph;

    graph.compile();

    CHECK(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("independent passes keep their insertion order")
{
    TestGraph graph;
    graph.addTexturePass({}, {TEXTURE_A});
    graph.addTexturePass({}, {TEXTURE_B});
    graph.addTexturePass({}, {TEXTURE_C});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("producers run before consumers, whatever order they were added in")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_B}, {});
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_B});
    graph.addTexturePass({}, {TEXTURE_A});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{2, 1, 0});
}

TEST_CASE("a diamond sorts its join after both branches")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_B, TEXTURE_C}, {});
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_B});
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_C});
    graph.addTexturePass({}, {TEXTURE_A});

    graph.compile();

    REQUIRE(graph.isCompiled());
    REQUIRE(graph.order().size() == 4);
    CHECK(positionOf(graph, 3) == 0);
    CHECK(positionOf(graph, 0) == 3);
}

TEST_CASE("buffers and render targets create edges too")
{
    const rasm::BufferHandle buffer{1, 1};
    const rasm::RenderTargetHandle renderTarget{1, 1};

    TestGraph graph;
    graph.addPass(
        "present",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(renderTarget);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(buffer);
            builder.write(renderTarget);
        },
        [](rasm::RenderContext &) {});
    graph.addComputePass(
        "cull",
        [&](rasm::PassBuilder &builder)
        { builder.write(buffer); },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{2, 1, 0});
}

TEST_CASE("reading a resource twice does not stall the sort")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_A, TEXTURE_A}, {});
    graph.addTexturePass({}, {TEXTURE_A});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{1, 0});
}

TEST_CASE("a pass reading its own output is not a cycle")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_A});

    graph.compile();

    CHECK(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0});
}

TEST_CASE("a cycle fails to compile")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_B}, {TEXTURE_A});
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_B});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("a cycle added after compiling drops the old order")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_B}, {TEXTURE_A});
    graph.compile();
    REQUIRE(graph.isCompiled());
    REQUIRE_FALSE(graph.order().empty());

    graph.addTexturePass({TEXTURE_A}, {TEXTURE_B});
    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("adding a pass after compiling triggers a new sort")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_A}, {});
    graph.compile();
    REQUIRE(graph.isCompiled());

    graph.addTexturePass({}, {TEXTURE_A});
    CHECK_FALSE(graph.isCompiled());

    graph.compile();
    CHECK(graph.order() == std::vector<uint32_t>{1, 0});
}

TEST_CASE("a pass that reads the render target it draws to fails to compile")
{
    const rasm::RenderTargetHandle renderTarget{1, 1};

    TestGraph graph;
    graph.addPass(
        "feedback",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(renderTarget);
            builder.write(renderTarget);
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("a compute pass that declares no write fails to compile")
{
    const rasm::BufferHandle buffer{1, 1};

    TestGraph graph;
    graph.addBufferPass(true, {buffer}, {});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("a compute pass sorts by its resources and writes no swapchain")
{
    const rasm::BufferHandle buffer{1, 1};
    const rasm::RenderTargetHandle renderTarget{1, 1};

    TestGraph graph;
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(buffer);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addComputePass(
        "cull",
        [&](rasm::PassBuilder &builder)
        {
            builder.write(buffer);
            builder.write(renderTarget);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{1, 0});
    CHECK(graph.pass(1).compute);
    CHECK_FALSE(graph.pass(0).compute);
    // Without an engine the graph can't see a target's desc, so it takes every target for a storage one.
    CHECK(graph.pass(1).outputRenderTarget.isValid());
    CHECK_FALSE(graph.pass(1).writesSwapchain);
}

TEST_CASE("a compute pass that writes a storage target runs before the passes that read it")
{
    const rasm::RenderTargetHandle bloom{1, 1};

    TestGraph graph;
    graph.addPass(
        "tone map",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(bloom);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addComputePass("bloom", [&](rasm::PassBuilder &builder)
                         { builder.write(bloom); }, [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{1, 0});
}

TEST_CASE("a compute pass that writes only a texture compiles")
{
    TestGraph graph;
    graph.addComputePass("bake", [&](rasm::PassBuilder &builder)
                         { builder.write(TEXTURE_A); }, [](rasm::RenderContext &) {});

    graph.compile();

    CHECK(graph.isCompiled());
}

TEST_CASE("an async compute pass writes no render target")
{
    const rasm::RenderTargetHandle target{1, 1};

    TestGraph graph;
    graph.addAsyncComputePass("bloom", [&](rasm::PassBuilder &builder)
                              { builder.write(target); }, [](rasm::RenderContext &) {});

    CHECK_FALSE(graph.pass(0).outputRenderTarget.isValid());
    graph.compile();
    // The dropped write leaves the pass without any write.
    CHECK_FALSE(graph.isCompiled());
}

TEST_CASE("a compute write is a transient storage target's first use")
{
    const rasm::RenderTargetHandle bloom{1, 1};

    TestGraph graph;
    graph.addTransient(bloom);
    graph.addComputePass("bloom", [&](rasm::PassBuilder &builder)
                         { builder.write(bloom); }, [](rasm::RenderContext &) {});
    graph.addPass(
        "tone map",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(bloom);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.firstPass(0) == 0);
    CHECK(graph.lastPass(0) == 1);
}

TEST_CASE("a graphics pass must write exactly one target")
{
    const rasm::RenderTargetHandle renderTarget{1, 1};
    TestGraph graph;

    SUBCASE("no target fails to compile")
    {
        graph.addPass(
            "nowhere",
            [](rasm::PassBuilder &) {},
            [](rasm::RenderContext &) {});
    }

    SUBCASE("a render target and the swapchain fail to compile")
    {
        graph.addPass(
            "both",
            [&](rasm::PassBuilder &builder)
            {
                builder.write(renderTarget);
                builder.writeSwapchain();
            },
            [](rasm::RenderContext &) {});
    }

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("passes that draw to the swapchain run in the order they were added")
{
    const rasm::RenderTargetHandle scene{1, 1};

    TestGraph graph;
    graph.addPass(
        "post",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(scene);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "ui",
        [](rasm::PassBuilder &builder)
        { builder.writeSwapchain(); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{2, 0, 1});
}

TEST_CASE("passes that draw to one render target run in the order they were added, before its readers")
{
    const rasm::RenderTargetHandle scene{1, 1};

    TestGraph graph;
    graph.addPass(
        "present",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(scene);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "opaque",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "transparent",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{1, 2, 0});
}

TEST_CASE("reading the previous frame adds no edge, even from the pass that draws the target")
{
    const rasm::RenderTargetHandle history{1, 1};

    TestGraph graph;
    graph.addPass(
        "reproject",
        [&](rasm::PassBuilder &builder)
        {
            builder.readPrevious(history);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "accumulate",
        [&](rasm::PassBuilder &builder)
        {
            builder.readPrevious(history);
            builder.write(history);
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0, 1});
    CHECK(graph.pass(1).previousRenderTargets == std::vector<rasm::RenderTargetHandle>{history});
}

TEST_CASE("a buffer barrier goes only where a pass uses a buffer another pass wrote or read")
{
    const rasm::BufferHandle particles{1, 1};
    const rasm::BufferHandle other{2, 1};
    constexpr bool COMPUTE = true;
    constexpr bool GRAPHICS = false;

    TestGraph graph;
    graph.addBufferPass(COMPUTE, {}, {particles});  // 0: animate
    graph.addBufferPass(GRAPHICS, {particles}, {}); // 1: draw
    graph.addBufferPass(GRAPHICS, {particles}, {}); // 2: draw again
    graph.addBufferPass(COMPUTE, {other}, {});      // 3: an unrelated read
    graph.addBufferPass(GRAPHICS, {}, {particles}); // 4: overwrite

    rasm::PipelineStage before = {};
    rasm::PipelineStage after = {};

    SUBCASE("the first write of a buffer waits for nothing")
    {
        CHECK_FALSE(graph.barrierBefore(0, before, after));
    }

    SUBCASE("a read waits for the write, once per stage")
    {
        graph.barrierBefore(0, before, after);

        REQUIRE(graph.barrierBefore(1, before, after));
        CHECK(before == rasm::PipelineStage::COMPUTE);
        CHECK(after == rasm::PipelineStage::GRAPHICS);
        CHECK_FALSE(graph.barrierBefore(2, before, after));
    }

    SUBCASE("a read of a buffer nothing wrote waits for nothing")
    {
        CHECK_FALSE(graph.barrierBefore(3, before, after));
    }

    SUBCASE("a write waits for the write and the reads before it")
    {
        graph.barrierBefore(0, before, after);
        graph.barrierBefore(1, before, after);

        REQUIRE(graph.barrierBefore(4, before, after));
        CHECK(before == (rasm::PipelineStage::COMPUTE | rasm::PipelineStage::GRAPHICS));
        CHECK(after == rasm::PipelineStage::GRAPHICS);
    }

    SUBCASE("the next frame's write waits for this frame's reads")
    {
        graph.barrierBefore(0, before, after);
        graph.barrierBefore(1, before, after);

        REQUIRE(graph.barrierBefore(0, before, after));
        CHECK(before == (rasm::PipelineStage::COMPUTE | rasm::PipelineStage::GRAPHICS));
        CHECK(after == rasm::PipelineStage::COMPUTE);
    }
}

TEST_CASE("a transient render target lives from the first pass that uses it to the last")
{
    const rasm::RenderTargetHandle scene{1, 1};
    const rasm::RenderTargetHandle post{2, 1};
    const rasm::RenderTargetHandle unused{3, 1};

    TestGraph graph;
    graph.addTransient(scene);
    graph.addTransient(post);
    graph.addTransient(unused);
    graph.addPass(
        "present",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(post);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "post",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(scene);
            builder.write(post);
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.order() == std::vector<uint32_t>{2, 1, 0});
    CHECK(graph.firstPass(0) == 0);
    CHECK(graph.lastPass(0) == 1);
    CHECK(graph.firstPass(1) == 1);
    CHECK(graph.lastPass(1) == 2);
    CHECK(graph.firstPass(2) == 0);
    CHECK(graph.lastPass(2) == 2);
}

TEST_CASE("reading a transient render target that no pass draws first fails to compile")
{
    const rasm::RenderTargetHandle scene{1, 1};

    TestGraph graph;
    graph.addTransient(scene);
    graph.addPass(
        "present",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(scene);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("the first pass that draws a target clears it, and each later one loads what the one before drew")
{
    const rasm::RenderTargetHandle scene{1, 1};

    TestGraph graph;
    graph.addPass(
        "opaque",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "ui",
        [](rasm::PassBuilder &builder)
        { builder.writeSwapchain(); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "transparent",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "present",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(scene);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();
    REQUIRE(graph.isCompiled());

    const rasm::AttachmentOps &opaque = graph.pass(0).attachmentOps;
    const rasm::AttachmentOps &ui = graph.pass(1).attachmentOps;
    const rasm::AttachmentOps &transparent = graph.pass(2).attachmentOps;
    const rasm::AttachmentOps &present = graph.pass(3).attachmentOps;

    CHECK_FALSE(opaque.loadColor);
    CHECK_FALSE(opaque.loadDepth);
    CHECK(opaque.storeDepth);
    CHECK(transparent.loadColor);
    CHECK(transparent.loadDepth);
    CHECK_FALSE(transparent.storeDepth);

    // The swapchain has no depth.
    CHECK_FALSE(ui.loadColor);
    CHECK_FALSE(ui.storeDepth);
    CHECK(present.loadColor);
    CHECK_FALSE(present.loadDepth);
    CHECK_FALSE(present.storeDepth);
}

TEST_CASE("each face of a cube target clears in its first pass, and only the cube's first pass may drop the other faces")
{
    const rasm::RenderTargetHandle cube{1, 1};
    const auto drawFace = [&](uint32_t face)
    {
        return [&cube, face](rasm::PassBuilder &builder)
        { builder.write(cube, face); };
    };

    TestGraph graph;
    graph.addPass("face 0", drawFace(0), [](rasm::RenderContext &) {});
    graph.addPass("face 1", drawFace(1), [](rasm::RenderContext &) {});
    graph.addPass("face 0 again", drawFace(0), [](rasm::RenderContext &) {});
    graph.addPass(
        "shade",
        [&](rasm::PassBuilder &builder)
        {
            builder.readDepth(cube);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();
    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0, 1, 2, 3});

    const rasm::AttachmentOps &face0 = graph.pass(0).attachmentOps;
    const rasm::AttachmentOps &face1 = graph.pass(1).attachmentOps;
    const rasm::AttachmentOps &face0Again = graph.pass(2).attachmentOps;

    CHECK_FALSE(face0.loadDepth);
    CHECK(face0.firstWrite);
    CHECK(face0.storeDepth);
    // Face 1 has nothing to load yet, but the barrier before it must not drop face 0.
    CHECK_FALSE(face1.loadDepth);
    CHECK_FALSE(face1.firstWrite);
    CHECK(face1.storeDepth);
    CHECK(face0Again.loadDepth);
    CHECK_FALSE(face0Again.firstWrite);
    CHECK(face0Again.storeDepth);
}

TEST_CASE("a compute pass and a lone graphics pass get the default attachment ops")
{
    const rasm::BufferHandle buffer{1, 1};

    TestGraph graph;
    graph.addBufferPass(true, {}, {buffer});
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(buffer);
            builder.writeSwapchain();
        },
        [](rasm::RenderContext &) {});

    graph.compile();
    REQUIRE(graph.isCompiled());

    for (uint32_t i = 0; i < 2; ++i)
    {
        CHECK_FALSE(graph.pass(i).attachmentOps.loadColor);
        CHECK_FALSE(graph.pass(i).attachmentOps.loadDepth);
        CHECK_FALSE(graph.pass(i).attachmentOps.storeDepth);
    }
}

TEST_CASE("a buffer written twice in a frame orders each reader between the write before it and the one after")
{
    const rasm::BufferHandle draws{1, 1};

    TestGraph graph;
    graph.addBufferPass(true, {}, {draws});
    graph.addBufferPass(false, {draws}, {});
    graph.addBufferPass(true, {}, {draws});
    graph.addBufferPass(false, {draws}, {});

    graph.compile();

    CHECK(graph.order() == std::vector<uint32_t>{0, 1, 2, 3});
}

TEST_CASE("reading a buffer's previous frame runs before this frame's first write of it")
{
    const rasm::BufferHandle visibility{1, 1};
    const rasm::BufferHandle draws{2, 1};

    TestGraph graph;
    graph.addComputePass(
        "late",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(draws);
            builder.write(visibility);
        },
        [](rasm::RenderContext &) {});
    graph.addComputePass(
        "early",
        [&](rasm::PassBuilder &builder)
        {
            builder.readPrevious(visibility);
            builder.write(draws);
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{1, 0});

    rasm::PipelineStage before = {};
    rasm::PipelineStage after = {};
    CHECK_FALSE(graph.barrierBefore(1, before, after));
    graph.barrierBefore(0, before, after);
    CHECK(graph.barrierBefore(1, before, after));
    CHECK(before == (rasm::PipelineStage::COMPUTE));
}

TEST_CASE("a pass that reads and writes a buffer before a later writer is not a cycle")
{
    const rasm::BufferHandle visibility{1, 1};

    TestGraph graph;
    graph.addBufferPass(true, {visibility}, {visibility});
    graph.addBufferPass(true, {}, {visibility});

    graph.compile();

    CHECK(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0, 1});
}

TEST_CASE("a depth read runs between the passes that draw the target, and the pass before it stores the depth")
{
    const rasm::RenderTargetHandle scene{1, 1};
    const rasm::BufferHandle hiz{1, 1};

    TestGraph graph;
    graph.addTransient(scene);
    graph.addPass(
        "early",
        [&](rasm::PassBuilder &builder)
        { builder.write(scene); },
        [](rasm::RenderContext &) {});
    graph.addComputePass(
        "hiz",
        [&](rasm::PassBuilder &builder)
        {
            builder.readDepth(scene);
            builder.write(hiz);
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "late",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(hiz);
            builder.write(scene);
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0, 1, 2});
    CHECK(graph.pass(0).attachmentOps.storeDepth);
    CHECK(graph.pass(2).attachmentOps.loadColor);
    CHECK(graph.pass(2).attachmentOps.loadDepth);
    CHECK(graph.firstPass(0) == 0);
    CHECK(graph.lastPass(0) == 2);
}

TEST_CASE("a pass that reads the depth of the target it draws to fails to compile")
{
    const rasm::RenderTargetHandle scene{1, 1};

    TestGraph graph;
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        {
            builder.readDepth(scene);
            builder.write(scene);
        },
        [](rasm::RenderContext &) {});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
}

TEST_CASE("a per-frame buffer orders its passes through every slot, and an async compute pass sorts like a compute pass")
{
    const rasm::BufferHandle draws[rasm::MAX_FRAMES_IN_FLIGHT] = {{1, 1}, {2, 1}};
    constexpr bool ASYNC = true;

    TestGraph graph;
    graph.addFrameBufferPass(!ASYNC, draws); // 0: draw
    graph.addFrameBufferPass(ASYNC, draws);  // 1: cull

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{1, 0});
    CHECK(graph.pass(1).async);
    CHECK(graph.pass(1).compute);
    CHECK_FALSE(graph.pass(0).async);
}

TEST_CASE("a per-frame buffer's barrier covers only the frame's slot")
{
    const rasm::BufferHandle draws[rasm::MAX_FRAMES_IN_FLIGHT] = {{1, 1}, {2, 1}};
    constexpr bool ASYNC = true;
    constexpr bool GRAPHICS = false;

    TestGraph graph;
    graph.addFrameBufferPass(ASYNC, draws);        // 0: cull into slot 0, the slot of a graph without an engine
    graph.addFrameBufferPass(!ASYNC, draws);       // 1: draw from slot 0
    graph.addBufferPass(GRAPHICS, {draws[1]}, {}); // 2: read slot 1, which nothing wrote

    rasm::PipelineStage before = {};
    rasm::PipelineStage after = {};
    CHECK_FALSE(graph.barrierBefore(0, before, after));
    REQUIRE(graph.barrierBefore(1, before, after));
    CHECK(before == rasm::PipelineStage::COMPUTE);
    CHECK(after == rasm::PipelineStage::GRAPHICS);
    CHECK_FALSE(graph.barrierBefore(2, before, after));
}

TEST_CASE("the dump lists the passes in order and each resource's lifetime")
{
    TestGraph graph;
    graph.addTexturePass({TEXTURE_A}, {});          // 0: reads A, runs last
    graph.addTexturePass({}, {TEXTURE_A});          // 1: writes A, runs first
    graph.addTexturePass({TEXTURE_B}, {TEXTURE_C}); // 2: reads B, which nothing writes

    graph.compile();
    const std::string text = graph.dump();

    REQUIRE(graph.order() == std::vector<uint32_t>{1, 2, 0});
    CHECK(text.find("Render graph: 3 passes, in the order they run") != std::string::npos);
    CHECK(text.find("[0] pass (graphics)\n    reads:  -\n    writes: texture #1:1") != std::string::npos);
    CHECK(text.find("before: [2] pass") != std::string::npos);
    CHECK(text.find("    W-R  texture #1:1\n") != std::string::npos);
    CHECK(text.find("    .R.  texture #2:1\n") != std::string::npos);
}

TEST_CASE("the dump lists a swapchain pass without depth")
{
    TestGraph graph;
    graph.addPass(
        "draw",
        [](rasm::PassBuilder &builder)
        { builder.writeSwapchain(); },
        [](rasm::RenderContext &) {});

    graph.compile();
    const std::string text = graph.dump();

    CHECK(text.find("[0] draw (graphics)") != std::string::npos);
    CHECK(text.find("    writes: swapchain") != std::string::npos);
    CHECK(text.find("    attachments: clear color, no depth") != std::string::npos);
    CHECK(text.find("swapchain depth") == std::string::npos);
}

TEST_CASE("the inspect pass runs right after the last pass that writes what it reads")
{
    TestGraph graph;
    graph.addTexturePass({}, {TEXTURE_A});          // 0
    graph.addTexturePass({}, {TEXTURE_B});          // 1: independent, sorted before 2 and 3 otherwise
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_C}); // 2
    graph.addInspectPass(TEXTURE_A, {9, 1});        // 3

    graph.compile();

    REQUIRE(graph.isCompiled());
    CHECK(graph.order() == std::vector<uint32_t>{0, 3, 1, 2});
}

TEST_CASE("taking the inspect pass out gives back the graph without it")
{
    TestGraph graph;
    graph.addTexturePass({}, {TEXTURE_A});
    graph.addTexturePass({}, {TEXTURE_B});
    graph.addTexturePass({TEXTURE_A}, {TEXTURE_C});
    graph.compile();
    const std::vector<uint32_t> before = graph.order();
    graph.addInspectPass(TEXTURE_A, {9, 1});
    graph.compile();
    REQUIRE(graph.order().size() == 4);

    graph.remove(graph.inspect());
    graph.compile();

    CHECK(graph.inspect() == UINT32_MAX);
    CHECK(graph.passCount() == 3);
    CHECK(graph.order() == before);
}

TEST_CASE("taking out a pass before the inspect pass keeps its index pointing at it")
{
    TestGraph graph;
    graph.addTexturePass({}, {TEXTURE_A});
    graph.addInspectPass(TEXTURE_A, {9, 1});
    graph.addTexturePass({}, {TEXTURE_B});
    graph.remove(0);

    CHECK(graph.inspect() == 0);
    CHECK(graph.pass(0).name == "Inspect");
}
