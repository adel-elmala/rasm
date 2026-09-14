#include "rasm/core/light.h"
#include "rasm/core/engine.h"

namespace rasm
{

    LightHandle Engine::createLight(LightType type, glm::vec3 color, float intensity, glm::vec3 position, glm::vec3 direction)
    {
        LightHandle handle = getNextLightHandle();

        Light light{};
        light.handle = handle;
        light.type = type;
        light.color = color;
        light.intensity = intensity;
        light.position = position;
        light.direction = direction;

        registery.lights[handle.index] = light;

        return handle;
    }
}
