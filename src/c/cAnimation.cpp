#include "c/cApi.h"

#include "rasm/animation.h"
#include "rasm/renderer.h"

#include <string>
#include <vector>

using namespace rasm;

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h
rasm_result RASM_CALL rasm_animation_play_init(rasm_animation_play *play, uint32_t size)
try
{
    const rasm_animation_play defaults = c::toC(AnimationPlay{});
    return c::initSized(play, size, c::ANIMATION_PLAY_SIZE_1_1, &defaults, sizeof(defaults), __func__, "play");
}
RASM_C_CATCH_RESULT

void RASM_CALL rasm_set_animation_time(rasm_renderer *renderer, double seconds)
try
{
    if (Renderer *r = c::getRenderer(renderer, __func__))
        r->setAnimationTime(seconds);
}
RASM_C_CATCH_VOID

rasm_result RASM_CALL rasm_play_animation(rasm_renderer *renderer, rasm_entity entity, const rasm_animation_play *play)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_animation_play values = c::toC(AnimationPlay{});
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::readSized(play, c::ANIMATION_PLAY_SIZE_1_1, &values, sizeof(values), __func__, "play"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->playAnimation(c::toCpp(entity), c::toCpp(values));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_stop_animation(rasm_renderer *renderer, rasm_entity entity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->stopAnimation(c::toCpp(entity));
    return errors.result();
}
RASM_C_CATCH_RESULT

uint32_t RASM_CALL rasm_get_animation_count(rasm_renderer *renderer, rasm_mesh mesh)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? static_cast<uint32_t>(r->getAnimations(c::toCpp(mesh)).size()) : 0;
}
RASM_C_CATCH(0)

uint32_t RASM_CALL rasm_get_animation_name(rasm_renderer *renderer, rasm_mesh mesh, uint32_t index, char *buffer, uint32_t capacity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return c::copyString({}, buffer, capacity);
    const std::vector<AnimationClip> clips = r->getAnimations(c::toCpp(mesh));
    if (index >= clips.size())
    {
        c::reportError(__func__, ("index " + std::to_string(index) + " is past the mesh's " + std::to_string(clips.size()) + " clips.").c_str());
        return c::copyString({}, buffer, capacity);
    }
    return c::copyString(clips[index].name, buffer, capacity);
}
RASM_C_CATCH(0)

float RASM_CALL rasm_get_animation_duration(rasm_renderer *renderer, rasm_mesh mesh, uint32_t index)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return 0.0f;
    const std::vector<AnimationClip> clips = r->getAnimations(c::toCpp(mesh));
    if (index >= clips.size())
    {
        c::reportError(__func__, ("index " + std::to_string(index) + " is past the mesh's " + std::to_string(clips.size()) + " clips.").c_str());
        return 0.0f;
    }
    return clips[index].duration;
}
RASM_C_CATCH(0.0f)

int32_t RASM_CALL rasm_find_animation(rasm_renderer *renderer, rasm_mesh mesh, const char *name)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkPointer(name, __func__, "name"))
        return -1;
    return r->findAnimation(c::toCpp(mesh), name);
}
RASM_C_CATCH(-1)

rasm_result RASM_CALL rasm_set_morph_weights(rasm_renderer *renderer, rasm_entity entity, const float *weights, uint32_t count)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || (count > 0 && !c::checkPointer(weights, __func__, "weights")))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setMorphWeights(c::toCpp(entity), weights, count);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_clear_morph_weights(rasm_renderer *renderer, rasm_entity entity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->clearMorphWeights(c::toCpp(entity));
    return errors.result();
}
RASM_C_CATCH_RESULT

uint32_t RASM_CALL rasm_get_morph_target_count(rasm_renderer *renderer, rasm_mesh mesh)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? static_cast<uint32_t>(r->getMorphTargetNames(c::toCpp(mesh)).size()) : 0;
}
RASM_C_CATCH(0)

uint32_t RASM_CALL rasm_get_morph_target_name(rasm_renderer *renderer, rasm_mesh mesh, uint32_t index, char *buffer, uint32_t capacity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return c::copyString({}, buffer, capacity);
    const std::vector<std::string> names = r->getMorphTargetNames(c::toCpp(mesh));
    if (index >= names.size())
    {
        c::reportError(__func__, ("index " + std::to_string(index) + " is past the mesh's " + std::to_string(names.size()) + " morph targets.").c_str());
        return c::copyString({}, buffer, capacity);
    }
    return c::copyString(names[index], buffer, capacity);
}
RASM_C_CATCH(0)
// NOLINTEND(readability-identifier-naming)
