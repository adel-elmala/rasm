#include "rasm/core/material.h"
#include "spdlog/spdlog.h"

namespace rasm
{

    Material::Material() {}
    Material::Material(MaterialHandle materialHandle)
        : handle(materialHandle) {}
    Material::~Material() {}

    MaterialHandle Material::id() const {
        return handle;
    }

    void Material::setTexture(PbrSlot slot, TextureHandle texture) {
        const std::size_t index = toIndex(slot);
        if (index >= textures.size()) {
            spdlog::error("PbrSlot enum value exceeds textures array size.");
            return;
        }

        textures[index] = texture;
    }
    void Material::setFloat(PbrParam param, float value) {
        const std::size_t index = toIndex(param);
        if (index >= scalarParams.size()) {
            spdlog::error("PbrParam enum value exceeds scalarParams array size.");
            return;
        }

        scalarParams[index] = value;
    }

}
