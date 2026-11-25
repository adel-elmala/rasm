#include "renderer/picking.h"
#include "rasm/view.h"
#include "rasm/window.h"

#include "doctest/doctest.h"
#include "glm/gtc/matrix_transform.hpp"

#include <cmath>
#include <vector>

using rasm::EntityHandle;
using rasm::InstanceOwner;
using rasm::InstanceOwners;
using rasm::PickGroup;
using rasm::PickSample;
using rasm::ViewPixels;

namespace
{
    // rasm.slang's packPickNormal, for the round trip.
    uint32_t packNormal(glm::vec3 n)
    {
        n /= std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
        glm::vec2 e(n.x, n.y);
        if (n.z < 0.0f)
            e = (1.0f - glm::abs(glm::vec2(n.y, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        const glm::vec2 q = glm::round(glm::clamp(e * 0.5f + 0.5f, 0.0f, 1.0f) * 65535.0f);
        return static_cast<uint32_t>(q.x) | static_cast<uint32_t>(q.y) << 16;
    }

    glm::dvec2 toNdc(const glm::dmat4 &matrix, const glm::dvec4 &clip)
    {
        const glm::dvec4 moved = matrix * clip;
        return glm::dvec2(moved) / moved.w;
    }
}

TEST_CASE("getPickClipMatrix stretches the rect's pixels to the whole of NDC")
{
    const ViewPixels rect = {.x = 300, .y = 100, .width = 8, .height = 4};
    const glm::dmat4 clip = rasm::getPickClipMatrix(rect, 1280, 720);
    // The rect's corners, as NDC of the view (pixel row 0 is the top, NDC y = 1), with a w that is not 1.
    const double w = 3.5;
    const glm::dvec2 topLeft(2.0 * 300 / 1280 - 1.0, 1.0 - 2.0 * 100 / 720);
    const glm::dvec2 bottomRight(2.0 * 308 / 1280 - 1.0, 1.0 - 2.0 * 104 / 720);
    const glm::dvec2 a = toNdc(clip, glm::dvec4(topLeft * w, 0.2, w));
    const glm::dvec2 b = toNdc(clip, glm::dvec4(bottomRight * w, 0.2, w));
    CHECK(a.x == doctest::Approx(-1.0));
    CHECK(a.y == doctest::Approx(1.0));
    CHECK(b.x == doctest::Approx(1.0));
    CHECK(b.y == doctest::Approx(-1.0));
    // Depth and w stay, so the frustum's near and far planes are the view's.
    const glm::dvec4 moved = clip * glm::dvec4(0.1, 0.2, 0.3, w);
    CHECK(moved.z == doctest::Approx(0.3));
    CHECK(moved.w == doctest::Approx(w));
}

TEST_CASE("getPickArgument packs the skip mask and minAlpha in 1/255 steps")
{
    // Rounded down, so an alpha of exactly minAlpha is picked: 0.5 is 127 / 255.
    CHECK(rasm::getPickArgument({}) == (127u << 8));
    CHECK(rasm::getPickArgument({.minAlpha = 1.0f, .skip = rasm::PICK_SKIP_LABELS}) == (rasm::PICK_SKIP_LABELS | 255u << 8));
    CHECK(rasm::getPickArgument({.minAlpha = -2.0f}) == 0u);
    CHECK(rasm::getPickArgument({.minAlpha = NAN}) == (127u << 8));
}

TEST_CASE("unpackPickNormal gives back the shader's packed normal")
{
    const glm::vec3 normals[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {0.3f, -0.8f, -0.52f}, {-0.6f, 0.1f, 0.79f}};
    for (glm::vec3 n : normals)
    {
        n = glm::normalize(n);
        const glm::vec3 back = rasm::unpackPickNormal(packNormal(n));
        CHECK(glm::dot(back, n) > 0.99999f);
    }
}

TEST_CASE("groupPickSamples: one group per (row, feature), nearest the rect's centre first")
{
    const ViewPixels rect = {.x = 10, .y = 10, .width = 5, .height = 5};
    std::vector<PickSample> samples = {
        {.row = 4, .position = glm::vec3(0, 0, -10), .x = 10, .y = 10},
        {.row = 4, .position = glm::vec3(0, 0, -10), .x = 11, .y = 11},
        {.row = 2, .feature = 7, .position = glm::vec3(0, 0, -20), .x = 12, .y = 12}, // the centre
        {.row = 2, .feature = 8, .position = glm::vec3(0, 0, -20), .x = 14, .y = 14},
        {.row = 4, .position = glm::vec3(0, 0, -10), .x = 13, .y = 12},
    };
    std::vector<PickGroup> groups;
    rasm::groupPickSamples(samples, rect, groups);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].nearest.row == 2);
    CHECK(groups[0].nearest.feature == 7);
    CHECK(groups[0].pixels == 1);
    CHECK(groups[1].nearest.row == 4);
    CHECK(groups[1].pixels == 3);
    CHECK(groups[1].nearest.x == 13); // one pixel from the centre, nearer than (11, 11)
    CHECK(groups[2].nearest.feature == 8);
}

TEST_CASE("groupPickSamples: at the same distance from the centre, the nearer surface comes first")
{
    const ViewPixels rect = {.x = 0, .y = 0, .width = 3, .height = 1};
    std::vector<PickSample> samples = {{.row = 1, .position = glm::vec3(0, 0, -30), .x = 0}, {.row = 9, .position = glm::vec3(0, 0, -5), .x = 2}};
    std::vector<PickGroup> groups;
    rasm::groupPickSamples(samples, rect, groups);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].nearest.row == 9);
}

TEST_CASE("InstanceOwners: a frame slot keeps a row's owner until its own sync, after another entity took the row")
{
    InstanceOwners owners;
    owners.reset(16);
    const EntityHandle a{3, 1};
    const EntityHandle b{3, 2}; // a's index again, after a was destroyed
    // Slot 0's frame gives rows 5 and 6 to a, and uploads them.
    owners.assign(5, 2, a);
    owners.sync(0);
    CHECK(owners.get(0, 5).entityGeneration == 1);
    CHECK(owners.get(0, 6).subMesh == 1);
    // Slot 1 has not synced yet: its copy of the table does not hold a's rows.
    CHECK(owners.get(1, 5).entityGeneration == 0);
    owners.sync(1);
    CHECK(owners.get(1, 5).entityGeneration == 1);
    // a is destroyed and b takes row 5 in slot 0's next frame: slot 1's copy still holds a until its own sync.
    owners.assign(5, 1, b);
    owners.sync(0);
    CHECK(owners.get(0, 5).entityGeneration == 2);
    CHECK(owners.get(1, 5).entityGeneration == 1);
    CHECK(owners.get(1, 5).entityIndex == 3);
    owners.sync(1);
    CHECK(owners.get(1, 5).entityGeneration == 2);
    // A row past the table has no owner.
    CHECK(owners.get(0, 99).entityGeneration == 0);
}

TEST_CASE("getViewPixel: window points to the pixel of a view, on a HiDPI window too")
{
    const rasm::Window window = {.width = 640, .height = 360, .pixelWidth = 1280, .pixelHeight = 720};
    const rasm::ViewRect right = {.x = 0.5f, .y = 0.0f, .width = 0.5f, .height = 1.0f};
    uint32_t x = 0;
    uint32_t y = 0;
    REQUIRE(rasm::getViewPixel(window, right, 330.25f, 10.0f, x, y));
    CHECK(x == 20);
    CHECK(y == 20);
    CHECK_FALSE(rasm::getViewPixel(window, right, 100.0f, 10.0f, x, y));
    CHECK_FALSE(rasm::getViewPixel(window, right, 700.0f, 10.0f, x, y));
    CHECK_FALSE(rasm::getViewPixel(rasm::Window{}, right, 1.0f, 1.0f, x, y));
}
