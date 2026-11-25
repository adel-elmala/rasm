#include "rasm/light.h"
#include "rasm/renderer.h"
#include "renderer/lightClusters.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        // glTF KHR_lights_punctual: 0 <= inner < outer <= pi / 2. The checks are written so that NaN fails them.
        bool isValidSpotCone(LightType type, float innerConeAngle, float outerConeAngle)
        {
            if (type != LightType::SPOT || (innerConeAngle >= 0.0f && innerConeAngle < outerConeAngle && outerConeAngle <= 1.57079633f))
                return true;
            spdlog::error("A spot light needs 0 <= innerConeAngle < outerConeAngle <= pi / 2, got {} and {}.", innerConeAngle, outerConeAngle);
            return false;
        }
    }

    LightHandle Renderer::createLight(LightType type, glm::vec3 color, glm::dvec3 position, glm::vec3 direction, float intensity, float range, float innerConeAngle, float outerConeAngle)
    {
        if (!isValidSpotCone(type, innerConeAngle, outerConeAngle))
            return {};

        const HandleSlot slot = registry.lightHandles.allocate();
        if (slot.index == 0)
        {
            spdlog::warn("Exceeded maximum number of lights ({}, RendererConfig::maxLights).", config.maxLights);
            return {};
        }
        const LightHandle handle{slot.index, slot.generation};

        Light light{};
        light.handle = handle;
        light.type = type;
        light.color = color;
        light.intensity = intensity;
        light.position = position;
        light.direction = direction;
        light.range = range;
        light.innerConeAngle = innerConeAngle;
        light.outerConeAngle = outerConeAngle;

        registry.lights.resize(registry.lightHandles.size());
        registry.lights[handle.index] = light;
        writeLightRow(handle);

        return handle;
    }

    // Its scenes rebuild their light lists without it, and its row goes to the next light at once: each frame slot has
    // its own copy of the light table. A shadow made for it casts nothing from now on.
    void Renderer::destroyLight(LightHandle light)
    {
        if (!isLive(light))
        {
            spdlog::error("destroyLight: light handle {{{}, {}}} is invalid or was destroyed.", light.index, light.generation);
            return;
        }
        markLightListsDirty(light);
        if (light.index < gpuLights->lightScenes.size())
            gpuLights->lightScenes[light.index] = 0;
        registry.lights[light.index] = {};
        registry.lightHandles.release(light.index, light.generation);
    }

    Light Renderer::getLight(LightHandle light) const
    {
        if (!isLive(light))
        {
            spdlog::error("Invalid light handle {}.", light.index);
            return {};
        }
        return registry.lights[light.index];
    }

    // A light that gains or loses its range moves between the global and the local part of its scenes' lists. Its type
    // stays: a shadow's maps (addShadowPasses) are made for it, cascades, one map or a cube.
    void Renderer::updateLight(LightHandle light, const Light &values)
    {
        if (!isLive(light))
        {
            spdlog::error("Invalid light handle for updating a light.");
            return;
        }
        Light &lightRef = registry.lights[light.index];
        if (values.type != lightRef.type)
        {
            spdlog::error("updateLight cannot change light {}'s type; create a new light instead.", light.index);
            return;
        }
        if (!isValidSpotCone(values.type, values.innerConeAngle, values.outerConeAngle))
            return;

        const bool kindChanged = isGlobalLight(lightRef) != isGlobalLight(values);
        lightRef = values;
        lightRef.handle = light;
        writeLightRow(light);
        if (kindChanged)
            markLightListsDirty(light);
    }
}
