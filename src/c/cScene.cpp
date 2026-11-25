#include "c/cApi.h"

#include "rasm/mesh.h"
#include "rasm/renderer.h"
#include "rasm/renderSpace.h"

#include <cstring>
#include <vector>

using namespace rasm;

// NOLINTBEGIN(readability-identifier-naming): the parameters keep their names from rasm_c.h

namespace
{
    // A perspective of 45 degrees with an infinite far plane, and a 2 x 2 orthographic box.
    rasm_camera_projection getProjectionDefaults()
    {
        return {.size = sizeof(rasm_camera_projection),
                .type = RASM_PROJECTION_TYPE_PERSPECTIVE,
                .fov_y = 45.0f,
                .aspect = 1.0f,
                .near_z = 0.1f,
                .far_z = 0.0f,
                .left = -1.0f,
                .right = 1.0f,
                .bottom = -1.0f,
                .top = 1.0f};
    }
}

rasm_result RASM_CALL rasm_texture_desc_init(rasm_texture_desc *desc, uint32_t size)
try
{
    const rasm_texture_desc defaults = {.size = sizeof(rasm_texture_desc), .format = RASM_FORMAT_R8G8B8A8_SRGB};
    return c::initSized(desc, size, c::TEXTURE_DESC_SIZE_1_0, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_light_desc_init(rasm_light_desc *desc, uint32_t size)
try
{
    const rasm_light_desc defaults = c::toC(Light{.type = LightType::DIRECTIONAL});
    return c::initSized(desc, size, c::LIGHT_DESC_SIZE_1_0, &defaults, sizeof(defaults), __func__, "desc");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_scene_environment_init(rasm_scene_environment *environment, uint32_t size)
try
{
    const rasm_scene_environment defaults = c::toC(SceneEnvironment{});
    return c::initSized(environment, size, c::SCENE_ENVIRONMENT_SIZE_1_0, &defaults, sizeof(defaults), __func__, "environment");
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_camera_projection_init(rasm_camera_projection *projection, uint32_t size)
try
{
    const rasm_camera_projection defaults = getProjectionDefaults();
    return c::initSized(projection, size, c::CAMERA_PROJECTION_SIZE_1_0, &defaults, sizeof(defaults), __func__, "projection");
}
RASM_C_CATCH_RESULT

rasm_scene RASM_CALL rasm_create_scene(rasm_renderer *renderer)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? c::toC(r->createScene()) : rasm_scene{};
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_scene(rasm_renderer *renderer, rasm_scene scene)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyScene(c::toCpp(scene));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_add_entity_to_scene(rasm_renderer *renderer, rasm_scene scene, rasm_entity entity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->addEntityToScene(c::toCpp(scene), c::toCpp(entity));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_remove_entity_from_scene(rasm_renderer *renderer, rasm_scene scene, rasm_entity entity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->removeEntityFromScene(c::toCpp(scene), c::toCpp(entity));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_add_camera_to_scene(rasm_renderer *renderer, rasm_scene scene, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->addCameraToScene(c::toCpp(scene), c::toCpp(camera));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_add_light_to_scene(rasm_renderer *renderer, rasm_scene scene, rasm_light light)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->addLightToScene(c::toCpp(scene), c::toCpp(light));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_remove_light_from_scene(rasm_renderer *renderer, rasm_scene scene, rasm_light light)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->removeLightFromScene(c::toCpp(scene), c::toCpp(light));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_scene_environment(rasm_renderer *renderer, rasm_scene scene, const rasm_scene_environment *environment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_scene_environment values = c::toC(SceneEnvironment{});
    if (r == nullptr || !c::readSized(environment, c::SCENE_ENVIRONMENT_SIZE_1_0, &values, sizeof(values), __func__, "environment"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setSceneEnvironment(c::toCpp(scene), c::toCpp(values));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_entity RASM_CALL rasm_create_entity(rasm_renderer *renderer, const char *name, rasm_mesh mesh, rasm_material material, const rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return {};
    return c::toC(r->createEntity(name != nullptr ? name : "", c::toCpp(mesh), c::toCpp(material), transform != nullptr ? c::toCpp(*transform) : Transform{}));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_entity(rasm_renderer *renderer, rasm_entity entity)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyEntity(c::toCpp(entity));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_transform(rasm_renderer *renderer, rasm_entity entity, rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *transform = c::toC(r->getTransform(c::toCpp(entity)));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_update_transform(rasm_renderer *renderer, rasm_entity entity, const rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->updateTransform(c::toCpp(entity), c::toCpp(*transform));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_mesh RASM_CALL rasm_create_mesh(rasm_renderer *renderer, const rasm_vertex *vertices, uint32_t vertex_count, const uint32_t *indices, uint32_t index_count, const uint32_t *colors)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || (vertex_count > 0 && !c::checkPointer(vertices, __func__, "vertices")) || (index_count > 0 && !c::checkPointer(indices, __func__, "indices")))
        return {};
    // The vectors createMesh takes, as a C++ caller builds them: rasm_vertex has Vertex's layout.
    std::vector<Vertex> meshVertices(vertex_count);
    if (vertex_count > 0)
        std::memcpy(meshVertices.data(), vertices, sizeof(Vertex) * vertex_count);
    std::vector<uint32_t> meshIndices(indices, indices + index_count);
    std::vector<uint32_t> meshColors;
    if (colors != nullptr)
        meshColors.assign(colors, colors + vertex_count);
    return c::toC(r->createMesh(std::move(meshVertices), std::move(meshIndices), std::move(meshColors)));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_mesh(rasm_renderer *renderer, rasm_mesh mesh)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyMesh(c::toCpp(mesh));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_bool RASM_CALL rasm_is_mesh_loaded(rasm_renderer *renderer, rasm_mesh mesh)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->isLoaded(c::toCpp(mesh)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

uint32_t RASM_CALL rasm_pack_vertex_color(rasm_vec4 linear_color)
try
{
    return packVertexColor(c::toCpp(linear_color));
}
RASM_C_CATCH(0)

rasm_material RASM_CALL rasm_create_material(rasm_renderer *renderer, rasm_material_type type, const rasm_texture *textures, uint32_t texture_count, rasm_bool double_sided)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkEnum(type, RASM_MATERIAL_TYPE_UNLIT + 1, __func__, "type") || (texture_count > 0 && !c::checkPointer(textures, __func__, "textures")))
        return {};
    std::vector<TextureHandle> slots(texture_count);
    for (uint32_t i = 0; i < texture_count; ++i)
        slots[i] = c::toCpp(textures[i]);
    return c::toC(r->createMaterial(static_cast<MaterialType>(type), std::move(slots), double_sided != 0));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_get_material(rasm_renderer *renderer, rasm_material material, rasm_material_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(material.id, __func__, "material"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const rasm_material_desc values = c::toC(r->getMaterial(c::toCpp(material)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    return c::writeSized(desc, c::MATERIAL_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_update_material(rasm_renderer *renderer, rasm_material material, const rasm_material_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(material.id, __func__, "material"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const Material current = r->getMaterial(c::toCpp(material));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    // A field past the caller's size keeps its current value.
    rasm_material_desc values = c::toC(current);
    if (!c::readSized(desc, c::MATERIAL_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") || !c::checkEnum(values.type, RASM_MATERIAL_TYPE_UNLIT + 1, __func__, "desc->type") ||
        !c::checkEnum(values.alpha_mode, RASM_ALPHA_MODE_BLEND + 1, __func__, "desc->alpha_mode"))
        return RASM_ERROR_INVALID_ARGUMENT;
    r->updateMaterial(c::toCpp(material), c::toCpp(values, current));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_destroy_material(rasm_renderer *renderer, rasm_material material)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyMaterial(c::toCpp(material));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_texture RASM_CALL rasm_create_texture(rasm_renderer *renderer, const rasm_texture_desc *desc, const void *pixels)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_texture_desc values = {.format = RASM_FORMAT_R8G8B8A8_SRGB};
    if (r == nullptr || !c::readSized(desc, c::TEXTURE_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") || !c::checkPointer(pixels, __func__, "pixels"))
        return {};
    if (values.format != RASM_FORMAT_R8G8B8A8_SRGB && values.format != RASM_FORMAT_R8G8B8A8_UNORM)
    {
        c::reportError(__func__, "desc->format must be RASM_FORMAT_R8G8B8A8_SRGB (color) or RASM_FORMAT_R8G8B8A8_UNORM (data).");
        return {};
    }
    const TextureHandle texture = r->createTexture({.name = values.name != nullptr ? values.name : "", .width = values.width, .height = values.height, .format = static_cast<Format>(values.format)});
    if (!texture.isValid())
        return {};
    // The pixels go to the GPU with their mips made, as 21_renderer's texture does.
    if (!r->getRenderContext().fillTexture(texture, pixels))
    {
        r->destroyTexture(texture);
        return {};
    }
    return c::toC(texture);
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_texture(rasm_renderer *renderer, rasm_texture texture)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyTexture(c::toCpp(texture));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_bool RASM_CALL rasm_is_texture_loaded(rasm_renderer *renderer, rasm_texture texture)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->isLoaded(c::toCpp(texture)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)

rasm_light RASM_CALL rasm_create_light(rasm_renderer *renderer, const rasm_light_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_light_desc values = c::toC(Light{.type = LightType::DIRECTIONAL});
    if (r == nullptr || !c::readSized(desc, c::LIGHT_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") || !c::checkEnum(values.type, RASM_LIGHT_TYPE_SPOT + 1, __func__, "desc->type"))
        return {};
    const Light light = c::toCpp(values);
    return c::toC(r->createLight(light.type, light.color, light.position, light.direction, light.intensity, light.range, light.innerConeAngle, light.outerConeAngle));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_get_light(rasm_renderer *renderer, rasm_light light, rasm_light_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(light.id, __func__, "light"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const rasm_light_desc values = c::toC(r->getLight(c::toCpp(light)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    return c::writeSized(desc, c::LIGHT_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_update_light(rasm_renderer *renderer, rasm_light light, const rasm_light_desc *desc)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(light.id, __func__, "light"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    rasm_light_desc values = c::toC(r->getLight(c::toCpp(light)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    const rasm_light_desc current = values;
    if (!c::readSized(desc, c::LIGHT_DESC_SIZE_1_0, &values, sizeof(values), __func__, "desc") || !c::checkEnum(values.type, RASM_LIGHT_TYPE_SPOT + 1, __func__, "desc->type"))
        return RASM_ERROR_INVALID_ARGUMENT;
    // A caller without position_d (1.1) gives the position in floats: it is that float position, or the light's exact
    // one when the caller left the float part as rasm_get_light gave it.
    if (desc->size < c::LIGHT_DESC_SIZE_1_2)
    {
        const bool unchanged = std::memcmp(&values.position, &current.position, sizeof(rasm_vec3)) == 0;
        values.position_d = unchanged ? current.position_d : rasm_dvec3{0.0, 0.0, 0.0};
    }
    Light updated = c::toCpp(values);
    updated.handle = c::toCpp(light);
    r->updateLight(c::toCpp(light), updated);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_destroy_light(rasm_renderer *renderer, rasm_light light)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyLight(c::toCpp(light));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_transform_d(rasm_renderer *renderer, rasm_entity entity, rasm_transform_d *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *transform = c::toTransformD(r->getTransform(c::toCpp(entity)));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_update_transform_d(rasm_renderer *renderer, rasm_entity entity, const rasm_transform_d *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(entity.id, __func__, "entity") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->updateTransform(c::toCpp(entity), c::toCpp(*transform));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_light_position_d(rasm_renderer *renderer, rasm_light light, rasm_dvec3 *position)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(light.id, __func__, "light") || !c::checkPointer(position, __func__, "position"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const Light values = r->getLight(c::toCpp(light));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    *position = c::toC(values.position);
    return RASM_OK;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_light_position_d(rasm_renderer *renderer, rasm_light light, const rasm_dvec3 *position)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(light.id, __func__, "light") || !c::checkPointer(position, __func__, "position"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    Light values = r->getLight(c::toCpp(light));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    values.position = c::toCpp(*position);
    r->updateLight(c::toCpp(light), values);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_vec3 RASM_CALL rasm_to_render_position(rasm_dvec3 world_position, rasm_dvec3 origin)
try
{
    return c::toC(toRenderPosition(c::toCpp(world_position), c::toCpp(origin)));
}
RASM_C_CATCH({})

rasm_dvec3 RASM_CALL rasm_to_world_position(rasm_vec3 render_position, rasm_dvec3 origin)
try
{
    return c::toC(toWorldPosition(c::toCpp(render_position), c::toCpp(origin)));
}
RASM_C_CATCH({})

rasm_camera RASM_CALL rasm_create_camera(rasm_renderer *renderer, const rasm_camera_projection *projection, const rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    rasm_camera_projection values = getProjectionDefaults();
    if (r == nullptr || !c::readSized(projection, c::CAMERA_PROJECTION_SIZE_1_0, &values, sizeof(values), __func__, "projection") ||
        !c::checkEnum(values.type, RASM_PROJECTION_TYPE_ORTHOGRAPHIC + 1, __func__, "projection->type"))
        return {};
    return c::toC(r->createCamera(c::toCpp(values), transform != nullptr ? c::toCpp(*transform) : Transform{}));
}
RASM_C_CATCH({})

rasm_result RASM_CALL rasm_destroy_camera(rasm_renderer *renderer, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyCamera(c::toCpp(camera));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_camera_transform(rasm_renderer *renderer, rasm_camera camera, rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *transform = c::toC(r->getCameraTransform(c::toCpp(camera)));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_camera_transform(rasm_renderer *renderer, rasm_camera camera, const rasm_transform *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setCameraTransform(c::toCpp(camera), c::toCpp(*transform));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_camera_transform_d(rasm_renderer *renderer, rasm_camera camera, rasm_transform_d *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *transform = c::toTransformD(r->getCameraTransform(c::toCpp(camera)));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_camera_transform_d(rasm_renderer *renderer, rasm_camera camera, const rasm_transform_d *transform)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(transform, __func__, "transform"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setCameraTransform(c::toCpp(camera), c::toCpp(*transform));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_camera_projection(rasm_renderer *renderer, rasm_camera camera, rasm_camera_projection *projection)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    const rasm_camera_projection values = c::toC(r->getCameraProjection(c::toCpp(camera)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    return c::writeSized(projection, c::CAMERA_PROJECTION_SIZE_1_0, &values, sizeof(values), __func__, "projection") ? RASM_OK : RASM_ERROR_INVALID_ARGUMENT;
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_set_camera_projection(rasm_renderer *renderer, rasm_camera camera, const rasm_camera_projection *projection)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    // A field past the caller's size keeps its current value.
    rasm_camera_projection values = c::toC(r->getCameraProjection(c::toCpp(camera)));
    if (errors.result() != RASM_OK)
        return RASM_ERROR_FAILED;
    if (!c::readSized(projection, c::CAMERA_PROJECTION_SIZE_1_0, &values, sizeof(values), __func__, "projection") ||
        !c::checkEnum(values.type, RASM_PROJECTION_TYPE_ORTHOGRAPHIC + 1, __func__, "projection->type"))
        return RASM_ERROR_INVALID_ARGUMENT;
    r->setCameraProjection(c::toCpp(camera), c::toCpp(values));
    return errors.result();
}
RASM_C_CATCH_RESULT

float RASM_CALL rasm_get_camera_exposure(rasm_renderer *renderer, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? r->getCameraExposure(c::toCpp(camera)) : 0.0f;
}
RASM_C_CATCH(0.0f)

rasm_result RASM_CALL rasm_set_camera_exposure(rasm_renderer *renderer, rasm_camera camera, float exposure)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setCameraExposure(c::toCpp(camera), exposure);
    return errors.result();
}
RASM_C_CATCH_RESULT

float RASM_CALL rasm_get_camera_bloom_strength(rasm_renderer *renderer, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr ? r->getCameraBloomStrength(c::toCpp(camera)) : 0.0f;
}
RASM_C_CATCH(0.0f)

rasm_result RASM_CALL rasm_set_camera_bloom_strength(rasm_renderer *renderer, rasm_camera camera, float strength)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->setCameraBloomStrength(c::toCpp(camera), strength);
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_reset_camera_history(rasm_renderer *renderer, rasm_camera camera)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->resetCameraHistory(c::toCpp(camera));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_get_view_matrix(rasm_renderer *renderer, rasm_camera camera, rasm_mat4 *matrix)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(matrix, __func__, "matrix"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *matrix = c::toC(glm::mat4(r->getViewMatrix(c::toCpp(camera))));
    return errors.result();
}
RASM_C_CATCH_RESULT

// aspect 0 keeps the camera's own aspect ratio.
rasm_result RASM_CALL rasm_get_projection_matrix(rasm_renderer *renderer, rasm_camera camera, float aspect, rasm_mat4 *matrix)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr || !c::checkHandle(camera.id, __func__, "camera") || !c::checkPointer(matrix, __func__, "matrix"))
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    *matrix = c::toC(aspect > 0.0f ? r->getProjectionMatrix(c::toCpp(camera), aspect) : r->getProjectionMatrix(c::toCpp(camera)));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_result RASM_CALL rasm_destroy_environment(rasm_renderer *renderer, rasm_environment environment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    if (r == nullptr)
        return RASM_ERROR_INVALID_ARGUMENT;
    const c::CallErrors errors;
    r->destroyEnvironment(c::toCpp(environment));
    return errors.result();
}
RASM_C_CATCH_RESULT

rasm_bool RASM_CALL rasm_is_environment_loaded(rasm_renderer *renderer, rasm_environment environment)
try
{
    Renderer *r = c::getRenderer(renderer, __func__);
    return r != nullptr && r->isLoaded(c::toCpp(environment)) ? RASM_TRUE : RASM_FALSE;
}
RASM_C_CATCH(RASM_FALSE)
// NOLINTEND(readability-identifier-naming)
