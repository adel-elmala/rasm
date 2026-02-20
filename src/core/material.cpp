#include "rasm/core/material.h"
#include <assert.h>
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
        assert(index < textures.size(), "PbrSlot enum value exceeds textures array size.");

        textures[index] = texture;
    }
    void Material::setFloat(PbrParam param, float value) {
        const std::size_t index = toIndex(param);
        assert(index < scalarParams.size(), "PbrParam enum value exceeds scalarParams array size.");

        scalarParams[index] = value;
    }

}
