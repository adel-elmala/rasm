#include "rasm/light.h"
#include "rasm/engine.h"
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

    LightHandle Engine::createLight(LightType type, glm::vec3 color, glm::vec3 position, glm::vec3 direction, float intensity, float range, float innerConeAngle, float outerConeAngle)
    {
        if (!isValidSpotCone(type, innerConeAngle, outerConeAngle))
            return {};

        LightHandle handle = handleManager.getNextLightHandle();

        if (handle.index > config.maxLights)
        {
            spdlog::warn("Exceeded maximum number of lights ({}, EngineConfig::maxLights).", config.maxLights);
            return {};
        }

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

        registry.lights.resize(handle.index + 1);
        registry.lights[handle.index] = light;
        writeLightRow(handle);

        return handle;
    }

    Light Engine::getLight(LightHandle light) const
    {
        if (!light.isValid() || light.index >= registry.lights.size())
        {
            spdlog::error("Invalid light handle {}.", light.index);
            return {};
        }
        return registry.lights[light.index];
    }

    // A light that gains or loses its range moves between the global and the local part of its scenes' lists.
    void Engine::updateLight(LightHandle light, const Light &values)
    {
        if (!light.isValid() || light.index >= registry.lights.size())
        {
            spdlog::error("Invalid light handle for updating a light.");
            return;
        }
        if (!isValidSpotCone(values.type, values.innerConeAngle, values.outerConeAngle))
            return;

        Light &lightRef = registry.lights[light.index];
        const bool kindChanged = isGlobalLight(lightRef) != isGlobalLight(values);
        lightRef = values;
        lightRef.handle = light;
        writeLightRow(light);
        if (kindChanged)
            markLightListsDirty(light);
    }
}
