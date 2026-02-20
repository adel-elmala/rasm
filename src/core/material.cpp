#include "rasm/core/material.h"

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
        textures[toIndex(slot)] = texture;
    }
    void Material::setFloat(PbrParam param, float value) {
        scalarParams[toIndex(param)] = value;
    }

}
