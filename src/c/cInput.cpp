#include "c/cApi.h"

#include "rasm/cameraController.h"
#include "rasm/cameraInput.h"
#include "rasm/renderer.h"
#include "rasm/view.h"

#include <cmath>
#include <memory>
#include <string>

using namespace rasm;

namespace
{
    // The window an id names: 0 is the main window, as in C++. A removed one is an empty Window: false, with an error.
    bool getWindowOf(Renderer &renderer, rasm_window window, const Window *&state, const char *function)
    {
        state = &renderer.getWindow(c::toCpp(window));
        if (state->handle.isValid())
            return true;
        c::reportError(function, "the window was removed.");
        return false;
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_window_desc_init(rasm_window_desc *desc, uint32_t size)
try
{
    const rasm_window_desc defaults = {.size = sizeof(rasm_window_desc), .native = c::toC(NativeWindow{})};
    return c::initSized(desc, size, c::WINDOW_DESC_SIZE_1_1, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_input_event_init(rasm_input_event *event, uint32_t size)
try
{
    const rasm_input_event defaults = c::toC(InputEvent{});
    return c::initSized(event, size, c::INPUT_EVENT_SIZE_1_1, &defaults, sizeof(defaults), __func__, "event");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_input_init(rasm_camera_input *input, uint32_t size)
try
{
    const rasm_camera_input defaults = c::toC(CameraInput{});
    return c::initSized(input, size, c::CAMERA_INPUT_SIZE_1_1, &defaults, sizeof(defaults), __func__, "input");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_controller_init(rasm_camera_controller *controller, uint32_t size)
try
{
    const rasm_camera_controller defaults = c::getControllerDefaults();
    return c::initSized(controller, size, c::CAMERA_CONTROLLER_SIZE_1_1, &defaults, sizeof(defaults), __func__, "controller");
}
RASM_C_CATCH_RESULT

rasm_window RASM_CALL rasm_get_main_window(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? c::toC(r->getMainWindow().handle) : rasm_window{};
}
RASM_C_CATCH({})

rasm_window RASM_CALL rasm_add_window(rasm_renderer *renderer, const rasm_window_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_window_desc values = {.native = c::toC(NativeWindow{})};
    if (r == nullptr || !c::readSized(desc, c::WINDOW_DESC_SIZE_1_1, &values, sizeof(values), __func__, "desc") ||
        !c::checkEnum(values.native.type, RASM_NATIVE_WINDOW_TYPE_METAL + 1, __func__, "desc->native.type"))
        return {};
    WindowDesc windowDesc = {.width = values.width, .height = values.height, .native = c::toCpp(values.native), .hidden = values.hidden != 0};
    if (values.title != nullptr)
        windowDesc.title = values.title;
    return c::toC(r->addWindow(windowDesc));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_remove_window(rasm_renderer *renderer, rasm_window window)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(window.id, __func__, "window"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->removeWindow(c::toCpp(window));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_window(rasm_renderer *renderer, rasm_window window, rasm_window_info *info)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const Window *state = nullptr;
    if (!getWindowOf(*r, window, state, __func__))
        return RASM_ERROR_FAILED;
    const rasm_window_info values = c::toC(*state);
    return c::writeSized(info, c::WINDOW_INFO_SIZE_1_1, &values, sizeof(values), __func__, "info") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_window_size(rasm_renderer *renderer, rasm_window window, uint32_t width, uint32_t height, uint32_t pixel_width, uint32_t pixel_height)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setWindowSize(c::toCpp(window), width, height, pixel_width, pixel_height);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_window_content_scale(rasm_renderer *renderer, rasm_window window, float scale)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setWindowContentScale(c::toCpp(window), scale);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_readback RASM_CALL rasm_read_window_frame(rasm_renderer *renderer, rasm_window window, rasm_readback_format format)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(format, RASM_READBACK_FORMAT_RGBA32_FLOAT + 1, __func__, "format"))
        return {};
    return c::toC(r->readFrame(c::toCpp(window), static_cast<ReadbackFormat>(format)));
}
RASM_C_CATCH({})

void RASM_CALL rasm_poll_events(rasm_renderer *renderer)
try
{
    if (Renderer *r = c::getRenderer(renderer, __func__))
        r->pollEvents();
}
RASM_C_CATCH_VOID

rasm_result RASM_CALL rasm_add_input_event(rasm_renderer *renderer, const rasm_input_event *event)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_input_event values = c::toC(InputEvent{});
    if (r == nullptr || !c::readSized(event, c::INPUT_EVENT_SIZE_1_1, &values, sizeof(values), __func__, "event") || !c::checkInputEvent(values, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    r->addInputEvent(c::toCpp(values));
    return RASM_OK;
}
RASM_C_CATCH_RESULT

uint32_t RASM_CALL rasm_get_input_event_count(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return 0;
    uint32_t count = 0;
    r->getInputEvents(count);
    return count;
}
RASM_C_CATCH(0)

rasm_result RASM_CALL rasm_get_input_event(rasm_renderer *renderer, uint32_t index, rasm_input_event *event)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    uint32_t count = 0;
    const InputEvent *events = r->getInputEvents(count);
    if (index >= count)
    {
        c::reportError(__func__, ("index " + std::to_string(index) + " is past the frame's " + std::to_string(count) + " events.").c_str());
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    const rasm_input_event values = c::toC(events[index]);
    return c::writeSized(event, c::INPUT_EVENT_SIZE_1_1, &values, sizeof(values), __func__, "event") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_camera_input(rasm_renderer *renderer, rasm_camera_input *input)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_camera_input values = c::toC(r->getCameraInput());
    return c::writeSized(input, c::CAMERA_INPUT_SIZE_1_1, &values, sizeof(values), __func__, "input") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_ui_input_capture RASM_CALL rasm_get_ui_input_capture(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? c::toC(r->getUiInputCapture()) : rasm_ui_input_capture{};
}
RASM_C_CATCH({})

float RASM_CALL rasm_get_delta_time(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? r->getDeltaTime() : 0.0f;
}
RASM_C_CATCH(0.0f)

rasm_result RASM_CALL rasm_get_camera_controller(rasm_renderer *renderer, rasm_camera camera, rasm_camera_controller *controller)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const rasm_camera_controller values = c::toC(r->getCameraController(c::toCpp(camera)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    return c::writeSized(controller, c::CAMERA_CONTROLLER_SIZE_1_1, &values, sizeof(values), __func__, "controller") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_camera_controller(rasm_renderer *renderer, rasm_camera camera, const rasm_camera_controller *controller)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    // A field past the caller's size takes its default: the controller is replaced whole, as in C++. Only the
    // trackball's center keeps its double rest for a 1.1 caller that left the float part as it got it.
    rasm_camera_controller values = c::getControllerDefaults();
    if (!c::readSized(controller, c::CAMERA_CONTROLLER_SIZE_1_1, &values, sizeof(values), __func__, "controller"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (controller->size < c::CAMERA_CONTROLLER_SIZE_1_2 && values.type == RASM_CAMERA_CONTROLLER_TYPE_TRACK_BALL)
    {
        const CameraController current = r->getCameraController(c::toCpp(camera));
        if (current.type == CameraController::Type::TRACK_BALL)
            values.track_ball_center_d = c::keepRemainder(values.track_ball_center, c::toFloatPart(current.trackBall.center), c::toRemainder(current.trackBall.center));
    }
    if (!c::checkCameraController(values, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setCameraController(c::toCpp(camera), c::toCpp(values));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_update_camera(rasm_renderer *renderer, rasm_camera camera, const rasm_camera_input *input)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_camera_input values = c::toC(CameraInput{});
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::readSized(input, c::CAMERA_INPUT_SIZE_1_1, &values, sizeof(values), __func__, "input"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->updateCamera(c::toCpp(camera), c::toCpp(values));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_camera_rig *RASM_CALL rasm_camera_rig_create(const rasm_camera_controller *controller, const rasm_transform *transform)
try
{
    rasm_camera_controller values = c::getControllerDefaults();
    if (!c::readSized(controller, c::CAMERA_CONTROLLER_SIZE_1_1, &values, sizeof(values), __func__, "controller") || !c::checkCameraController(values, __func__))
        return nullptr;
    auto rig = std::make_unique<rasm_camera_rig>();
    rig->rig = createCameraRig(c::toCpp(values), transform != nullptr ? c::toCpp(*transform) : Transform{});
    return rig.release();
}
RASM_C_CATCH(nullptr)

void RASM_CALL rasm_camera_rig_destroy(rasm_camera_rig *rig)
try
{
    delete rig;
}
RASM_C_CATCH_VOID

rasm_result RASM_CALL rasm_camera_rig_update(rasm_camera_rig *rig, const rasm_camera_input *input)
try
{
    rasm_camera_input values = c::toC(CameraInput{});
    if (!c::checkPointer(rig, __func__, "rig") || !c::readSized(input, c::CAMERA_INPUT_SIZE_1_1, &values, sizeof(values), __func__, "input"))
        return RASM_ERROR_INVALID_ARGUMENT;
    updateCameraRig(rig->rig, c::toCpp(values));
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_camera_rig *RASM_CALL rasm_camera_rig_create_d(const rasm_camera_controller *controller, const rasm_transform_d *transform)
try
{
    rasm_camera_controller values = c::getControllerDefaults();
    if (!c::readSized(controller, c::CAMERA_CONTROLLER_SIZE_1_1, &values, sizeof(values), __func__, "controller") || !c::checkCameraController(values, __func__))
        return nullptr;
    auto rig = std::make_unique<rasm_camera_rig>();
    rig->rig = createCameraRig(c::toCpp(values), transform != nullptr ? c::toCpp(*transform) : Transform{});
    return rig.release();
}
RASM_C_CATCH(nullptr)

rasm_result RASM_CALL rasm_camera_rig_set_free_fly_up(rasm_camera_rig *rig, rasm_vec3 up)
try
{
    if (!c::checkPointer(rig, __func__, "rig"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z))
    {
        c::reportError(__func__, "up is not finite.");
        return RASM_ERROR_INVALID_ARGUMENT;
    }
    if (rig->rig.controller.type == CameraController::Type::FREE_FLY)
        rig->rig.controller.freeFly.up = c::toCpp(up);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_rig_get_transform_d(const rasm_camera_rig *rig, rasm_transform_d *transform, rasm_transform_d *target_transform)
try
{
    if (!c::checkPointer(rig, __func__, "rig"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (transform != nullptr)
        *transform = c::toTransformD(rig->rig.transform);
    if (target_transform != nullptr)
        *target_transform = c::toTransformD(rig->rig.targetTransform);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_rig_get_transform(const rasm_camera_rig *rig, rasm_transform *transform, rasm_transform *target_transform)
try
{
    if (!c::checkPointer(rig, __func__, "rig"))
        return RASM_ERROR_INVALID_ARGUMENT;
    if (transform != nullptr)
        *transform = c::toC(rig->rig.transform);
    if (target_transform != nullptr)
        *target_transform = c::toC(rig->rig.targetTransform);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_camera_input_state *RASM_CALL rasm_camera_input_state_create(void)
try
{
    return new rasm_camera_input_state();
}
RASM_C_CATCH(nullptr)

void RASM_CALL rasm_camera_input_state_destroy(rasm_camera_input_state *state)
try
{
    delete state;
}
RASM_C_CATCH_VOID

rasm_result RASM_CALL rasm_camera_input_state_add_event(rasm_camera_input_state *state, const rasm_input_event *event, const rasm_input_area *area, const rasm_ui_input_capture *capture)
try
{
    rasm_input_event values = c::toC(InputEvent{});
    if (!c::checkPointer(state, __func__, "state") || !c::checkPointer(area, __func__, "area") || !c::readSized(event, c::INPUT_EVENT_SIZE_1_1, &values, sizeof(values), __func__, "event") ||
        !c::checkInputEvent(values, __func__))
        return RASM_ERROR_INVALID_ARGUMENT;
    addCameraInputEvent(state->state, c::toCpp(values), c::toCpp(*area), capture != nullptr ? c::toCpp(*capture) : UiInputCapture{});
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_input_state_take(rasm_camera_input_state *state, const rasm_ui_input_capture *capture, float delta_time, rasm_camera_input *input)
try
{
    if (!c::checkPointer(state, __func__, "state"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const rasm_camera_input values = c::toC(takeCameraInput(state->state, capture != nullptr ? c::toCpp(*capture) : UiInputCapture{}, delta_time));
    return c::writeSized(input, c::CAMERA_INPUT_SIZE_1_1, &values, sizeof(values), __func__, "input") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_vec2 RASM_CALL rasm_to_input_ndc(float x, float y, const rasm_input_area *area)
try
{
    if (!c::checkPointer(area, __func__, "area"))
        return {};
    return c::toC(toInputNdc(x, y, c::toCpp(*area)));
}
RASM_C_CATCH({})

rasm_input_area RASM_CALL rasm_get_input_area(rasm_renderer *renderer, rasm_window window, const rasm_view_rect *rect)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return {};
    const Window *state = nullptr;
    if (!getWindowOf(*r, window, state, __func__))
        return {};
    return c::toC(getInputArea(*state, rect != nullptr ? c::toCpp(*rect) : ViewRect{}));
}
RASM_C_CATCH({})

rasm_bool RASM_CALL rasm_get_view_pixel(rasm_renderer *renderer, rasm_window window, const rasm_view_rect *rect, float x, float y, uint32_t *pixel_x, uint32_t *pixel_y)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    const Window *state = nullptr;
    if (r == nullptr || !c::checkPointer(pixel_x, __func__, "pixel_x") || !c::checkPointer(pixel_y, __func__, "pixel_y") || !getWindowOf(*r, window, state, __func__))
        return RASM_FALSE;
    return getViewPixel(*state, rect != nullptr ? c::toCpp(*rect) : ViewRect{}, x, y, *pixel_x, *pixel_y) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)
// NOLINTEND(readability-identifier-naming)
