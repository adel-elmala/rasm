#include "platform/input.h"
#include "rasm/cameraInput.h"
#include "rasm/engine.h"

#include "doctest/doctest.h"

#include <cmath>

using rasm::CameraInputState;
using rasm::InputArea;
using rasm::InputEvent;
using rasm::InputEventType;
using rasm::InputState;
using rasm::Key;
using rasm::UiInputCapture;

namespace
{
    constexpr float TOLERANCE = 1e-5f;
    constexpr InputArea WINDOW_800X600 = {.width = 800.0f, .height = 600.0f};

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

    InputEvent press(rasm::MouseButton which, float x, float y, bool down, uint8_t modifiers = 0)
    {
        return {.type = InputEventType::MOUSE_BUTTON, .down = down, .modifiers = modifiers, .button = which, .x = x, .y = y};
    }

    InputEvent move(float x, float y)
    {
        return {.type = InputEventType::MOUSE_MOVE, .x = x, .y = y};
    }

    InputEvent wheel(float steps)
    {
        return {.type = InputEventType::MOUSE_WHEEL, .y = steps};
    }
}

TEST_CASE("window points become NDC divided by the smaller side, with y up")
{
    const InputArea window = WINDOW_800X600;
    CHECK(isNear(rasm::toInputNdc(400.0f, 300.0f, window), glm::vec2(0.0f)));
    CHECK(isNear(rasm::toInputNdc(700.0f, 0.0f, window), glm::vec2(1.0f, 1.0f)));
    CHECK(isNear(rasm::toInputNdc(100.0f, 600.0f, window), glm::vec2(-1.0f, -1.0f)));
}

TEST_CASE("a drag gives the camera this frame's start and end, and the next frame starts where it ended")
{
    const InputArea window = WINDOW_800X600;
    CameraInputState state;
    rasm::addCameraInputEvent(state, button(400.0f, 300.0f, true), window, {});
    rasm::addCameraInputEvent(state, move(460.0f, 300.0f), window, {});
    rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.0f)));
    CHECK(input.deltaTime == doctest::Approx(0.016f));

    // No move this frame: the button is still down, and the drag does not jump.
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.2f, 0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.0f)));

    rasm::addCameraInputEvent(state, move(460.0f, 240.0f), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, glm::vec2(0.2f, 0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(0.2f, 0.2f)));

    // Released: moves no longer drag.
    rasm::addCameraInputEvent(state, button(460.0f, 240.0f, false), window, {});
    rasm::addCameraInputEvent(state, move(700.0f, 100.0f), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseStart, input.mouseEnd));
}

TEST_CASE("the button map decides which drag a button starts, and only its release ends it")
{
    const InputArea window = WINDOW_800X600;
    CameraInputState state;
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::RIGHT, 400.0f, 300.0f, true), window, {});
    rasm::addCameraInputEvent(state, move(460.0f, 300.0f), window, {});
    rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.panStart, glm::vec2(0.0f)));
    CHECK(isNear(input.panEnd, glm::vec2(0.2f, 0.0f)));
    CHECK(isNear(input.mouseStart, input.mouseEnd));
    CHECK(isNear(input.pointer, glm::vec2(0.2f, 0.0f)));

    // Another button during the drag changes nothing, and its release does not end the drag.
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::LEFT, 460.0f, 300.0f, true), window, {});
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::LEFT, 460.0f, 300.0f, false), window, {});
    rasm::addCameraInputEvent(state, move(520.0f, 300.0f), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.panEnd, glm::vec2(0.4f, 0.0f)));
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::RIGHT, 520.0f, 300.0f, false), window, {});
    CHECK(!state.dragActive);

    // Shift + left pans; with every button mapped to ORBIT, the right button orbits as before pans existed.
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::LEFT, 400.0f, 300.0f, true, rasm::KEY_MOD_SHIFT), window, {});
    CHECK(state.drag == rasm::CameraDrag::PAN);
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::LEFT, 400.0f, 300.0f, false), window, {});
    state.buttons = {.right = rasm::CameraDrag::ORBIT, .middle = rasm::CameraDrag::ORBIT, .back = rasm::CameraDrag::ORBIT, .forward = rasm::CameraDrag::ORBIT, .shiftLeft = rasm::CameraDrag::ORBIT};
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::RIGHT, 400.0f, 300.0f, true), window, {});
    rasm::addCameraInputEvent(state, move(400.0f, 240.0f), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseEnd, glm::vec2(0.0f, 0.2f)));
    CHECK(isNear(input.panStart, input.panEnd));
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::RIGHT, 400.0f, 240.0f, false), window, {});

    // A button mapped to NONE starts nothing.
    state.buttons.middle = rasm::CameraDrag::NONE;
    rasm::addCameraInputEvent(state, press(rasm::MouseButton::MIDDLE, 400.0f, 300.0f, true), window, {});
    CHECK(!state.dragActive);
}

TEST_CASE("a drag follows the motion: past the area's edge, and during a grab where the pointer stands still")
{
    const InputArea left = {.width = 400.0f, .height = 600.0f};
    CameraInputState state;
    rasm::addCameraInputEvent(state, button(350.0f, 300.0f, true), left, {});
    for (int i = 1; i <= 6; ++i)
    {
        // Steps of 30 points to the right: from the third on, the pointer is outside the area.
        rasm::addCameraInputEvent(state, move(350.0f + 30.0f * static_cast<float>(i), 300.0f), left, {});
        const rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
        CHECK(isNear(input.mouseEnd - input.mouseStart, glm::vec2(0.15f, 0.0f)));
    }
    // A grab: x and y stay, dx and dy carry the motion.
    InputEvent grabbed = move(350.0f, 300.0f);
    grabbed.dx = 40.0f;
    grabbed.dy = -20.0f;
    rasm::addCameraInputEvent(state, grabbed, left, {});
    rasm::addCameraInputEvent(state, grabbed, left, {});
    const rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(isNear(input.mouseEnd - input.mouseStart, glm::vec2(0.4f, 0.2f)));
}

TEST_CASE("what the UI captures does not reach the camera")
{
    const InputArea window = WINDOW_800X600;
    CameraInputState state;
    const UiInputCapture mouse = {.mouse = true};
    rasm::addCameraInputEvent(state, button(100.0f, 100.0f, true), window, mouse);
    rasm::addCameraInputEvent(state, move(200.0f, 100.0f), window, mouse);
    rasm::addCameraInputEvent(state, wheel(2.0f), window, mouse);
    rasm::CameraInput input = rasm::takeCameraInput(state, mouse, 0.016f);
    CHECK_FALSE(state.dragActive);
    CHECK(isNear(input.mouseStart, input.mouseEnd));
    CHECK(input.wheelDelta == 0.0f);
    // The pointer is still tracked, for the UI when it resumes.
    CHECK(state.pointerX == 200.0f);
    CHECK(state.pointerInWindow);

    const UiInputCapture keyboard = {.keyboard = true};
    rasm::addCameraInputEvent(state, key(Key::W, Key::W, true), window, keyboard);
    input = rasm::takeCameraInput(state, keyboard, 0.016f);
    CHECK_FALSE(input.forward);
    // Held, so it moves the camera once the UI lets go of the keyboard.
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(input.forward);
}

TEST_CASE("the camera moves by key position, and a key without one counts as its meaning")
{
    const InputArea window = WINDOW_800X600;
    CameraInputState state;
    // AZERTY: the key at W's place types Z.
    rasm::addCameraInputEvent(state, key(Key::Z, Key::W, true), window, {});
    rasm::addCameraInputEvent(state, key(Key::A, Key::NONE, true), window, {});
    rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(input.forward);
    CHECK(input.left);
    CHECK_FALSE(input.backward);

    rasm::addCameraInputEvent(state, key(Key::Z, Key::W, false), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK_FALSE(input.forward);
    CHECK(input.left);

    // A key or button released in another window never sends its release: losing the focus lets go of it.
    rasm::addCameraInputEvent(state, button(400.0f, 300.0f, true), window, {});
    rasm::addCameraInputEvent(state, {.type = InputEventType::FOCUS, .down = false}, window, {});
    rasm::addCameraInputEvent(state, move(500.0f, 300.0f), window, {});
    input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK_FALSE(input.left);
    CHECK_FALSE(state.dragActive);
    CHECK(isNear(input.mouseStart, input.mouseEnd));
}

TEST_CASE("an area maps its own center to NDC 0, and only a press in it starts a drag")
{
    // The right half of an 800 x 600 window, as one of two side-by-side views.
    const InputArea right = {.x = 400.0f, .y = 0.0f, .width = 400.0f, .height = 600.0f};
    CHECK(isNear(rasm::toInputNdc(600.0f, 300.0f, right), glm::vec2(0.0f)));
    CHECK(isNear(rasm::toInputNdc(800.0f, 100.0f, right), glm::vec2(1.0f, 1.0f)));

    CameraInputState state;
    rasm::addCameraInputEvent(state, button(100.0f, 300.0f, true), right, {});
    rasm::addCameraInputEvent(state, move(500.0f, 300.0f), right, {});
    CHECK_FALSE(state.dragActive);
    rasm::addCameraInputEvent(state, button(500.0f, 300.0f, false), right, {});

    // A drag that starts in the area goes on outside it, until the release.
    rasm::addCameraInputEvent(state, button(600.0f, 300.0f, true), right, {});
    rasm::addCameraInputEvent(state, move(200.0f, 300.0f), right, {});
    rasm::CameraInput input = rasm::takeCameraInput(state, {}, 0.016f);
    CHECK(state.dragActive);
    CHECK(isNear(input.mouseStart, glm::vec2(0.0f)));
    CHECK(isNear(input.mouseEnd, glm::vec2(-2.0f, 0.0f)));
}

TEST_CASE("the wheel counts only with the pointer in the area")
{
    const InputArea right = {.x = 400.0f, .y = 0.0f, .width = 400.0f, .height = 600.0f};
    CameraInputState state;
    // No pointer seen yet: wheel events come only over the window, so it counts.
    rasm::addCameraInputEvent(state, wheel(1.0f), right, {});
    CHECK(rasm::takeCameraInput(state, {}, 0.016f).wheelDelta == doctest::Approx(1.0f));
    rasm::addCameraInputEvent(state, move(100.0f, 300.0f), right, {});
    rasm::addCameraInputEvent(state, wheel(1.0f), right, {});
    CHECK(rasm::takeCameraInput(state, {}, 0.016f).wheelDelta == 0.0f);

    rasm::addCameraInputEvent(state, move(600.0f, 300.0f), right, {});
    rasm::addCameraInputEvent(state, wheel(2.0f), right, {});
    rasm::addCameraInputEvent(state, wheel(0.5f), right, {});
    CHECK(rasm::takeCameraInput(state, {}, 0.016f).wheelDelta == doctest::Approx(2.5f));
    CHECK(rasm::takeCameraInput(state, {}, 0.016f).wheelDelta == 0.0f);

    rasm::addCameraInputEvent(state, {.type = InputEventType::MOUSE_LEAVE}, right, {});
    rasm::addCameraInputEvent(state, wheel(1.0f), right, {});
    CHECK(rasm::takeCameraInput(state, {}, 0.016f).wheelDelta == 0.0f);
}

TEST_CASE("the engine's keys follow the layout, count once per press, and wait while a text field types")
{
    const InputArea window = WINDOW_800X600;
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

TEST_CASE("pollEvents applies the events added before it, once per frame, and getInputEvents returns them in order")
{
    // Zero window dimensions: the engine stops before it opens a window or touches the GPU.
    rasm::Engine engine(rasm::EngineConfig{.cacheDir = "", .fixedDeltaTime = 0.02f});
    engine.addInputEvent(key(Key::W, Key::W, true));
    engine.addInputEvent(move(10.0f, 20.0f));
    uint32_t count = 0;
    CHECK(engine.getInputEvents(count) != nullptr);
    CHECK(count == 0);

    engine.pollEvents();
    const InputEvent *events = engine.getInputEvents(count);
    REQUIRE(count == 2);
    CHECK(events[0].type == InputEventType::KEY);
    CHECK(events[1].type == InputEventType::MOUSE_MOVE);
    CHECK(engine.getCameraInput().forward);
    CHECK(engine.getCameraInput().deltaTime == doctest::Approx(0.02f));
    CHECK(engine.getDeltaTime() == doctest::Approx(0.02f));
    CHECK_FALSE(engine.getUiInputCapture().mouse);

    // A second poll in the same frame does nothing: the new event waits for the next frame.
    engine.addInputEvent(key(Key::W, Key::W, false));
    engine.pollEvents();
    engine.getInputEvents(count);
    CHECK(count == 2);
    CHECK(engine.getCameraInput().forward);
}
