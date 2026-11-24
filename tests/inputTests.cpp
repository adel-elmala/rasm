#include "platform/input.h"

#include "doctest/doctest.h"

#include <cmath>

using rasm::InputEvent;
using rasm::InputEventType;
using rasm::InputState;
using rasm::Key;
using rasm::UiInputCapture;

namespace
{
    constexpr float TOLERANCE = 1e-5f;

    rasm::Window window800x600()
    {
        rasm::Window window;
        window.width = 800;
        window.height = 600;
        window.pixelWidth = 1600;
        window.pixelHeight = 1200;
        return window;
    }

    bool isNear(const glm::vec2 &a, const glm::vec2 &b)
    {
        return std::abs(a.x - b.x) < TOLERANCE && std::abs(a.y - b.y) < TOLERANCE;
    }

    InputEvent key(Key key, Key physicalKey, bool down, bool repeat = false)
    {
        return {.type = InputEventType::KEY, .key = key, .physicalKey = physicalKey, .down = down, .repeat = repeat};
    }

    InputEvent button(float x, float y, bool down)
    {
        return {.type = InputEventType::MOUSE_BUTTON, .down = down, .x = x, .y = y};
    }

    InputEvent move(float x, float y)
    {
        return {.type = InputEventType::MOUSE_MOVE, .x = x, .y = y};
    }
}

TEST_CASE("window points become NDC divided by the smaller side, with y up")
{
    const rasm::Window window = window800x600();
    CHECK(isNear(rasm::toInputNdc(400.0f, 300.0f, window), glm::vec2(0.0f)));
    CHECK(isNear(rasm::toInputNdc(700.0f, 0.0f, window), glm::vec2(1.0f, 1.0f)));
    CHECK(isNear(rasm::toInputNdc(100.0f, 600.0f, window), glm::vec2(-1.0f, -1.0f)));
}

TEST_CASE("a drag gives the camera this frame's start and end, and the next frame starts where it ended")
{
    const rasm::Window window = window800x600();
    InputState state;
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, button(400.0f, 300.0f, true), window, {});
    rasm::applyInputEvent(state, move(460.0f, 300.0f), window, {});
    rasm::CameraInput input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.0f)));
    CHECK(input.deltaTime == doctest::Approx(0.016f));

    // No move this frame: the button is still down, and the drag does not jump.
    rasm::beginInputFrame(state);
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.2f, 0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.0f)));

    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, move(460.0f, 240.0f), window, {});
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.2f, 0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.2f)));

    // Released: moves no longer drag.
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, button(460.0f, 240.0f, false), window, {});
    rasm::applyInputEvent(state, move(700.0f, 100.0f), window, {});
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, input.mouseEnd));
}

TEST_CASE("what the UI captures does not reach the camera")
{
    const rasm::Window window = window800x600();
    InputState state;
    const UiInputCapture mouse = {.mouse = true};
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, button(100.0f, 100.0f, true), window, mouse);
    rasm::applyInputEvent(state, move(200.0f, 100.0f), window, mouse);
    rasm::applyInputEvent(state, {.type = InputEventType::MOUSE_WHEEL, .y = 2.0f}, window, mouse);
    rasm::CameraInput input = rasm::endInputFrame(state, mouse, 0.016f);
    CHECK_FALSE(state.dragActive);
    CHECK(isNear(input.mouseStart, input.mouseEnd));
    CHECK(input.wheelDelta == 0.0f);
    // The pointer is still tracked, for the UI when it resumes.
    CHECK(state.mouseX == 200.0f);
    CHECK(state.mouseInside);

    const UiInputCapture keyboard = {.keyboard = true};
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, key(Key::W, Key::W, true), window, keyboard);
    input = rasm::endInputFrame(state, keyboard, 0.016f);
    CHECK_FALSE(input.forward);
    // Held, so it moves the camera once the UI lets go of the keyboard.
    rasm::beginInputFrame(state);
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(input.forward);
}

TEST_CASE("the camera moves by key position, and a key without one counts as its meaning")
{
    const rasm::Window window = window800x600();
    InputState state;
    // AZERTY: the key at W's place types Z.
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, key(Key::Z, Key::W, true), window, {});
    rasm::applyInputEvent(state, key(Key::A, Key::NONE, true), window, {});
    rasm::CameraInput input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK(input.forward);
    CHECK(input.left);
    CHECK_FALSE(input.backward);

    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, key(Key::Z, Key::W, false), window, {});
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK_FALSE(input.forward);
    CHECK(input.left);

    // A key released in another window never sends its release: losing the focus lets go of it.
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, {.type = InputEventType::FOCUS, .down = false}, window, {});
    input = rasm::endInputFrame(state, {}, 0.016f);
    CHECK_FALSE(input.left);
}

TEST_CASE("the engine's keys follow the layout, count once per press, and wait while a text field types")
{
    const rasm::Window window = window800x600();
    InputState state;
    rasm::beginInputFrame(state);
    rasm::applyInputEvent(state, key(Key::F1, Key::F1, true), window, {});
    rasm::applyInputEvent(state, key(Key::F1, Key::F1, true, true), window, {});
    rasm::applyInputEvent(state, key(Key::F2, Key::NONE, true), window, {});
    rasm::applyInputEvent(state, key(Key::F3, Key::F3, true), window, {});
    rasm::applyInputEvent(state, key(Key::ESCAPE, Key::ESCAPE, true), window, {});
    CHECK(state.statsToggles == 1);
    CHECK(state.forcedLodSteps == 1);
    CHECK(state.dumpGraph);
    CHECK(state.escapes == 1);

    rasm::beginInputFrame(state);
    CHECK(state.statsToggles == 0);
    CHECK_FALSE(state.dumpGraph);
    const UiInputCapture text = {.keyboard = true, .text = true};
    rasm::applyInputEvent(state, key(Key::F1, Key::F1, true), window, text);
    rasm::applyInputEvent(state, key(Key::ESCAPE, Key::ESCAPE, true), window, text);
    CHECK(state.statsToggles == 0);
    CHECK(state.escapes == 0);
}
