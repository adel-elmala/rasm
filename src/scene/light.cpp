#include "rasm/light.h"
#include "rasm/engine.h"

#include "spdlog/spdlog.h"

namespace rasm
{

    LightHandle Engine::createLight(LightType type, glm::vec3 color, glm::vec3 position, glm::vec3 direction, float intensity, float range, float innerConeAngle, float outerConeAngle)
    {
        // glTF KHR_lights_punctual: 0 <= inner < outer <= pi / 2. The checks are written so that NaN fails them.
        if (type == LightType::SPOT && !(innerConeAngle >= 0.0f && innerConeAngle < outerConeAngle && outerConeAngle <= 1.57079633f))
        {
            spdlog::error("A spot light needs 0 <= innerConeAngle < outerConeAngle <= pi / 2, got {} and {}.", innerConeAngle, outerConeAngle);
            return {};
        }

        LightHandle handle = handleManager.getNextLightHandle();

        if (handle.index >= MAX_LIGHTS)
        {
            spdlog::warn("Exceeded maximum number of lights.");
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

        return handle;
    }
}
