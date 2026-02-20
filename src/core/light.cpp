#include "rasm/core/light.h"

namespace rasm
{

    Light::Light(LightType type) : type(type) {}
    Light::~Light() {}

    void Light::setColor(const glm::vec3 &color) {
        // TODO: Store the color and use it during rendering.
    }
    void Light::setIntensity(float intensity) {
        // TODO: Store the intensity and use it during rendering.
    }
}
