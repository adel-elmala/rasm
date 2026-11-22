#include "renderer/inspector.h"

#include "doctest/doctest.h"

// The Buffers section exists only in a rasm built with Dear ImGui (RASM_IMGUI).
#if defined(RASM_IMGUI)

TEST_CASE("a source fills the capture target's region with its aspect kept")
{
    CHECK(rasm::getInspectRegion({1280, 720}, 1024) == glm::uvec2(1024, 576));
    CHECK(rasm::getInspectRegion({2048, 2048}, 1024) == glm::uvec2(1024, 1024));
    CHECK(rasm::getInspectRegion({720, 1280}, 1024) == glm::uvec2(576, 1024));
    // A small source is scaled up, so each texel is a block.
    CHECK(rasm::getInspectRegion({16, 9}, 1024) == glm::uvec2(1024, 576));
    // A thin one keeps at least one texel across.
    CHECK(rasm::getInspectRegion({4096, 1}, 1024) == glm::uvec2(1024, 1));
    CHECK(rasm::getInspectRegion({0, 0}, 1024) == glm::uvec2(1024, 1024));
}

TEST_CASE("a Hi-Z level halves the one before, rounded up, from half the viewport")
{
    CHECK(rasm::getHizLevelSize({1280, 720}, 0) == glm::uvec2(640, 360));
    CHECK(rasm::getHizLevelSize({1280, 720}, 3) == glm::uvec2(80, 45));
    CHECK(rasm::getHizLevelSize({1281, 721}, 0) == glm::uvec2(641, 361));
    CHECK(rasm::getHizLevelSize({1280, 720}, 4) == glm::uvec2(40, 23));
    CHECK(rasm::getHizLevelSize({1, 1}, 5) == glm::uvec2(1, 1));
}

#endif
