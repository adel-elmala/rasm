#include "renderer/labelPlacement.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

namespace
{
    constexpr uint32_t SLOTS = 16;
    constexpr uint32_t WIDTH = 640;
    constexpr uint32_t HEIGHT = 360;

    rasm::LabelCandidate makeCandidate(uint32_t slot, float priority, const glm::vec2 &center, bool inView = true)
    {
        const glm::vec2 half(40.0f, 10.0f);
        return {.slot = slot, .priority = priority, .low = center - half, .high = center + half, .inView = inView};
    }
}

TEST_CASE("placeLabels: the higher priority wins an overlap, and the first placement shows at once")
{
    rasm::LabelPlacement state;
    const std::vector<uint32_t> stamps(SLOTS, 1);
    rasm::LabelCandidate candidates[3] = {makeCandidate(0, 1.0f, {100.0f, 100.0f}), makeCandidate(1, 5.0f, {120.0f, 105.0f}), makeCandidate(2, 0.0f, {400.0f, 300.0f})};
    rasm::placeLabels(state, candidates, 3, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.placed[1] == 1);
    CHECK(state.placed[0] == 0);
    CHECK(state.placed[2] == 1);
    CHECK(state.alpha[1] == 1.0f);
    CHECK(state.alpha[0] == 0.0f);
    CHECK(state.alpha[2] == 1.0f);
}

TEST_CASE("placeLabels: equal priorities keep the label placed last frame, and fade by step")
{
    rasm::LabelPlacement state;
    const std::vector<uint32_t> stamps(SLOTS, 1);
    // Slot 5 alone first.
    rasm::LabelCandidate alone[2] = {makeCandidate(5, 1.0f, {200.0f, 200.0f}), makeCandidate(2, 1.0f, {210.0f, 200.0f}, false)};
    rasm::placeLabels(state, alone, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    REQUIRE(state.placed[5] == 1);
    // Slot 2 comes into view over it: a lower slot, the same priority, but slot 5 was placed.
    rasm::LabelCandidate both[2] = {makeCandidate(5, 1.0f, {200.0f, 200.0f}), makeCandidate(2, 1.0f, {210.0f, 200.0f})};
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        rasm::placeLabels(state, both, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
        CHECK(state.placed[5] == 1);
        CHECK(state.placed[2] == 0);
    }
    // Slot 5 moves away: slot 2 fades in by a quarter a frame.
    both[0] = makeCandidate(5, 1.0f, {500.0f, 50.0f});
    rasm::placeLabels(state, both, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.placed[2] == 1);
    CHECK(state.alpha[2] == doctest::Approx(0.25f));
    rasm::placeLabels(state, both, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.alpha[2] == doctest::Approx(0.5f));
    // Out of view, it fades out.
    both[1].inView = false;
    rasm::placeLabels(state, both, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.placed[2] == 0);
    CHECK(state.alpha[2] == doctest::Approx(0.25f));
}

TEST_CASE("placeLabels: a slot with a new stamp holds a new label, which starts hidden")
{
    rasm::LabelPlacement state;
    std::vector<uint32_t> stamps(SLOTS, 1);
    rasm::LabelCandidate candidates[1] = {makeCandidate(3, 0.0f, {100.0f, 100.0f})};
    rasm::placeLabels(state, candidates, 1, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.alpha[3] == 1.0f);
    stamps[3] = 2;
    rasm::placeLabels(state, candidates, 1, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.alpha[3] == doctest::Approx(0.25f));
}

TEST_CASE("placeLabels: a box past the screen's edges is clipped to it, one wholly off it is not placed")
{
    rasm::LabelPlacement state;
    const std::vector<uint32_t> stamps(SLOTS, 1);
    rasm::LabelCandidate candidates[2] = {makeCandidate(0, 0.0f, {10.0f, 5.0f}), makeCandidate(1, 0.0f, {WIDTH + 100.0f, 100.0f})};
    rasm::placeLabels(state, candidates, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.placed[0] == 1);
    CHECK(state.placed[1] == 0);
}

TEST_CASE("placeLabels: a steady frame keeps the state's capacity")
{
    rasm::LabelPlacement state;
    const std::vector<uint32_t> stamps(SLOTS, 1);
    rasm::LabelCandidate candidates[2] = {makeCandidate(0, 0.0f, {100.0f, 100.0f}), makeCandidate(1, 0.0f, {300.0f, 100.0f})};
    rasm::placeLabels(state, candidates, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    const float *alpha = state.alpha.data();
    const uint64_t *cells = state.cells.data();
    const uint32_t *order = state.order.data();
    rasm::placeLabels(state, candidates, 2, stamps.data(), SLOTS, WIDTH, HEIGHT, 0.25f);
    CHECK(state.alpha.data() == alpha);
    CHECK(state.cells.data() == cells);
    CHECK(state.order.data() == order);
}
