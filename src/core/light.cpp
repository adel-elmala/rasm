#include "rasm/core/light.h"

namespace rasm
{

    Light::Light(LightType type) : type(type) {}
    Light::~Light() {}

    void Light::setColor(const glm::vec3 &color) {
        (void)color;
    }
    void Light::setIntensity(float intensity) {
        (void)intensity;
    }
}
