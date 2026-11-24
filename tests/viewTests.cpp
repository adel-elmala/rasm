#include "rasm/view.h"

#include "doctest/doctest.h"

using rasm::getInputArea;
using rasm::getViewPixels;
using rasm::InputArea;
using rasm::ViewPixels;
using rasm::ViewRect;
using rasm::Window;

TEST_CASE("getViewPixels covers the whole image by default")
{
    CHECK(getViewPixels({}, 1280, 720) == ViewPixels{.x = 0, .y = 0, .width = 1280, .height = 720});
}

TEST_CASE("getViewPixels: rects that touch share their edge, with no gap or overlap")
{
    // Thirds of an odd width do not divide evenly; the rounded edges still tile it.
    const ViewPixels left = getViewPixels({.x = 0.0f, .width = 1.0f / 3.0f}, 1001, 10);
    const ViewPixels middle = getViewPixels({.x = 1.0f / 3.0f, .width = 1.0f / 3.0f}, 1001, 10);
    const ViewPixels right = getViewPixels({.x = 2.0f / 3.0f, .width = 1.0f / 3.0f}, 1001, 10);
    CHECK(left.x == 0);
    CHECK(middle.x == left.x + left.width);
    CHECK(right.x == middle.x + middle.width);
    CHECK(right.x + right.width == 1001);
}

TEST_CASE("getViewPixels: a picture-in-picture in the top-right corner")
{
    CHECK(getViewPixels({.x = 0.75f, .y = 0.0f, .width = 0.25f, .height = 0.25f}, 1280, 720) == ViewPixels{.x = 960, .y = 0, .width = 320, .height = 180});
}

TEST_CASE("getViewPixels keeps at least one pixel inside the image")
{
    CHECK(getViewPixels({.x = 0.5f, .y = 0.5f, .width = 0.0f, .height = 0.0f}, 100, 100) == ViewPixels{.x = 50, .y = 50, .width = 1, .height = 1});
    CHECK(getViewPixels({.x = 1.0f, .y = 1.0f, .width = 0.5f, .height = 0.5f}, 100, 100) == ViewPixels{.x = 99, .y = 99, .width = 1, .height = 1});
    CHECK(getViewPixels({.x = -0.5f, .y = 0.0f, .width = 2.0f, .height = 1.0f}, 100, 100) == ViewPixels{.x = 0, .y = 0, .width = 100, .height = 100});
    CHECK(getViewPixels({}, 0, 100) == ViewPixels{});
}

TEST_CASE("getInputArea is the rect in the window's points")
{
    Window window{};
    window.width = 800;
    window.height = 600;
    window.pixelWidth = 1600;
    window.pixelHeight = 1200;
    const InputArea area = getInputArea(window, {.x = 0.5f, .y = 0.25f, .width = 0.5f, .height = 0.5f});
    CHECK(area.x == doctest::Approx(400.0f));
    CHECK(area.y == doctest::Approx(150.0f));
    CHECK(area.width == doctest::Approx(400.0f));
    CHECK(area.height == doctest::Approx(300.0f));
}
