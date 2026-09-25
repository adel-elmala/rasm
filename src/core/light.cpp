#include "rasm/core/light.h"
#include "rasm/core/engine.h"

namespace rasm
{

    LightHandle Engine::createLight(LightType type, glm::vec3 color, float intensity, glm::vec3 position, glm::vec3 direction)
    {
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

        registery.lights.resize(handle.index + 1);
        registery.lights[handle.index] = light;

        return handle;
    }
}
