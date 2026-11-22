#include "renderer/ui.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

// The UI exists only in a rasm built with Dear ImGui (RASM_IMGUI).
#if defined(RASM_IMGUI)

TEST_CASE("UI scissor: a clip rectangle becomes framebuffer pixels")
{
    const float origin[2] = {0.0f, 0.0f};
    const float scale[2] = {1.0f, 1.0f};
    rasm::UiScissor scissor;

    SUBCASE("inside the framebuffer, fractions round outward")
    {
        const float clip[4] = {10.5f, 20.0f, 100.2f, 50.0f};
        CHECK(rasm::getUiScissor(clip, origin, scale, 1280, 720, scissor));
        CHECK(scissor.x == 10);
        CHECK(scissor.y == 20);
        CHECK(scissor.width == 91);
        CHECK(scissor.height == 30);
    }
    SUBCASE("clamped to the framebuffer")
    {
        const float clip[4] = {-50.0f, -10.0f, 2000.0f, 900.0f};
        CHECK(rasm::getUiScissor(clip, origin, scale, 1280, 720, scissor));
        CHECK(scissor.x == 0);
        CHECK(scissor.y == 0);
        CHECK(scissor.width == 1280);
        CHECK(scissor.height == 720);
    }
    SUBCASE("outside or empty: nothing to draw")
    {
        const float outside[4] = {1300.0f, 0.0f, 1400.0f, 100.0f};
        CHECK_FALSE(rasm::getUiScissor(outside, origin, scale, 1280, 720, scissor));
        const float empty[4] = {100.0f, 100.0f, 100.0f, 200.0f};
        CHECK_FALSE(rasm::getUiScissor(empty, origin, scale, 1280, 720, scissor));
    }
    SUBCASE("points times the framebuffer scale, from the display's origin")
    {
        const float clip[4] = {110.0f, 60.0f, 210.0f, 110.0f};
        const float displayPos[2] = {100.0f, 50.0f};
        const float retina[2] = {2.0f, 2.0f};
        CHECK(rasm::getUiScissor(clip, displayPos, retina, 2560, 1440, scissor));
        CHECK(scissor.x == 20);
        CHECK(scissor.y == 20);
        CHECK(scissor.width == 200);
        CHECK(scissor.height == 100);
    }
}

TEST_CASE("UI buffers grow in powers of two from their minimum")
{
    CHECK(rasm::growUiCapacity(0, 65536) == 65536);
    CHECK(rasm::growUiCapacity(65536, 65536) == 65536);
    CHECK(rasm::growUiCapacity(65537, 65536) == 131072);
    CHECK(rasm::growUiCapacity(1000000, 65536) == 1048576);
}

TEST_CASE("UI textures are released once the graphics fence passed the last frame that may draw them")
{
    std::vector<rasm::UiRelease> releases = {{.slot = 3, .fenceValue = 10}, {.slot = 4, .fenceValue = 11}};
    std::vector<rasm::UiRelease> done;

    // A frame that was built but never submitted moves no fence, so nothing goes early.
    rasm::takeUiReleases(releases, 9, done);
    CHECK(done.empty());
    CHECK(releases.size() == 2);

    rasm::takeUiReleases(releases, 10, done);
    REQUIRE(done.size() == 1);
    CHECK(done[0].slot == 3);
    REQUIRE(releases.size() == 1);
    CHECK(releases[0].slot == 4);

    rasm::takeUiReleases(releases, 12, done);
    CHECK(done.size() == 2);
    CHECK(releases.empty());
}

#endif
