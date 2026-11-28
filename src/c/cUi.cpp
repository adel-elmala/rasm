#include "c/cApi.h"

#include "rasm/renderer.h"

#if defined(RASM_IMGUI)
#include "imgui.h"
#include "renderer/ui.h"
#endif

#include <string>

using namespace rasm;

namespace
{
    // The renderer, when its UI is on.
    Renderer *getUiRenderer(rasm_renderer *renderer, const char *function)
    {
        Renderer *r = c::getRenderer(renderer, function);
        if (r != nullptr && r->getUiContext() == nullptr)
        {
            c::reportError(function, "the UI is off: it needs rasm_renderer_config's ui, and a RASM built with Dear ImGui.");
            return nullptr;
        }
        return r;
    }

    // The C callback runs as a recording frame's, so the calls that would break the frame are refused inside it.
    void callUiCallback(rasm_renderer *renderer)
    {
        const c::Recording recording(*renderer);
        renderer->uiCallback(renderer->uiUserData);
    }
}

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_set_ui_callback(rasm_renderer *renderer, rasm_ui_callback callback, void *user_data)
try
{
    Renderer *r = getUiRenderer(renderer, __func__);
    if (r == nullptr)
        return renderer == nullptr ? RASM_ERROR_INVALID_ARGUMENT : RASM_ERROR_FAILED;
    // Inside the UI callback this would also free the function that runs.
    if (!c::checkNotRecording(renderer, __func__))
        return RASM_ERROR_FAILED;
    renderer->uiCallback = callback;
    renderer->uiUserData = user_data;
    if (callback == nullptr)
        r->setUiCallback({});
    else
        r->setUiCallback([renderer]
                         { callUiCallback(renderer); });
    return RASM_OK;
}
RASM_C_CATCH_RESULT

uint64_t RASM_CALL rasm_get_ui_texture(rasm_renderer *renderer, rasm_texture texture)
try
{
    Renderer *r = getUiRenderer(renderer, __func__);
    return r != nullptr ? r->getUiTexture(c::toCpp(texture)) : 0;
}
RASM_C_CATCH(0)

void *RASM_CALL rasm_get_imgui_context(rasm_renderer *renderer)
try
{
    Renderer *r = getUiRenderer(renderer, __func__);
    return r != nullptr ? r->getUiContext() : nullptr;
}
RASM_C_CATCH(nullptr)

rasm_result RASM_CALL rasm_get_imgui_allocator_functions(rasm_imgui_alloc_func *alloc_func, rasm_imgui_free_func *free_func, void **user_data)
try
{
    if (!c::checkPointer(alloc_func, __func__, "alloc_func") || !c::checkPointer(free_func, __func__, "free_func") || !c::checkPointer(user_data, __func__, "user_data"))
        return RASM_ERROR_INVALID_ARGUMENT;
#if defined(RASM_IMGUI)
    ImGuiMemAllocFunc alloc = nullptr;
    ImGuiMemFreeFunc free = nullptr;
    ImGui::GetAllocatorFunctions(&alloc, &free, user_data);
    *alloc_func = alloc;
    *free_func = free;
    return RASM_OK;
#else
    *alloc_func = nullptr;
    *free_func = nullptr;
    *user_data = nullptr;
    c::reportError(__func__, "RASM was built without Dear ImGui (RASM_IMGUI=OFF).");
    return RASM_ERROR_FAILED;
#endif
}
RASM_C_CATCH_RESULT

const char *RASM_CALL rasm_get_imgui_version(uint32_t *version_num)
try
{
#if defined(RASM_IMGUI)
    if (version_num != nullptr)
        *version_num = IMGUI_VERSION_NUM;
    return IMGUI_VERSION;
#else
    if (version_num != nullptr)
        *version_num = 0;
    return nullptr;
#endif
}
RASM_C_CATCH(nullptr)

rasm_result RASM_CALL rasm_check_imgui_layout(const char *version, uint64_t io_size, uint64_t style_size, uint64_t vec2_size, uint64_t vec4_size, uint64_t draw_vert_size,
                                              uint64_t draw_idx_size)
try
{
    if (!c::checkPointer(version, __func__, "version"))
        return RASM_ERROR_INVALID_ARGUMENT;
#if defined(RASM_IMGUI)
    const UiLayout caller = {.version = version, .io = io_size, .style = style_size, .vec2 = vec2_size, .vec4 = vec4_size, .drawVert = draw_vert_size, .drawIdx = draw_idx_size};
    const std::string mismatch = findUiLayoutMismatch(getUiLayout(), caller, "the caller's");
    if (mismatch.empty())
        return RASM_OK;
    c::reportError(__func__, mismatch.c_str());
#else
    (void)io_size, (void)style_size, (void)vec2_size, (void)vec4_size, (void)draw_vert_size, (void)draw_idx_size;
    c::reportError(__func__, "RASM was built without Dear ImGui (RASM_IMGUI=OFF).");
#endif
    return RASM_ERROR_FAILED;
}
RASM_C_CATCH_RESULT
// NOLINTEND(readability-identifier-naming)
