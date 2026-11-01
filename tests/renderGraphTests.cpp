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
                },
                [](rasm::RenderContext &) {});
        }

        const std::vector<uint32_t> &order() const { return executionOrder; }
        bool isCompiled() const { return compiled; }
    };

    size_t positionOf(const TestGraph &graph, uint32_t passIdx)
    {
        const auto &order = graph.order();
        return std::find(order.begin(), order.end(), passIdx) - order.begin();
    }

    const rasm::TextureHandle TEXTURE_A{1, 1};
    const rasm::TextureHandle TEXTURE_B{2, 1};
    const rasm::TextureHandle TEXTURE_C{3, 1};
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
        { builder.read(renderTarget); },
        [](rasm::RenderContext &) {});
    graph.addPass(
        "draw",
        [&](rasm::PassBuilder &builder)
        {
            builder.read(buffer);
            builder.write(renderTarget);
        },
        [](rasm::RenderContext &) {});
    graph.addPass(
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
