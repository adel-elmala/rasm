#include "rasm/render/renderGraph.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    // compile() never touches the engine, so a graph without one can be sorted but not executed.
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

        // What execute records before the pass: false for no barrier.
        bool barrierBefore(uint32_t passIdx, rasm::PipelineStage &before, rasm::PipelineStage &after) { return bufferBarrier(passes[passIdx], before, after); }

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

TEST_CASE("a compute pass that declares no buffer write fails to compile")
{
    const rasm::BufferHandle buffer{1, 1};

    TestGraph graph;
    graph.addBufferPass(true, {buffer}, {});

    graph.compile();

    CHECK_FALSE(graph.isCompiled());
    CHECK(graph.order().empty());
}

TEST_CASE("a compute pass sorts by its resources and writes no render target")
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
    CHECK_FALSE(graph.pass(1).outputRenderTarget.isValid());
    CHECK_FALSE(graph.pass(1).writesSwapchain);
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
