// clang-format off
#pragma once

#include "core.slang"

// Base color only (KHR_materials_unlit), without lights. The tone-map pass still maps it, like every color of the scene.
float4 shadeUnlit(Sampler2D gTextures[], MetallicRoughnessMaterial material, float2 baseColorUV)
{
    return sampleBaseColor(gTextures, material, baseColorUV);
}

// clang-format on
