// clang-format off
#pragma once

#include "core.slang"

// Base color only (KHR_materials_unlit). Not tone mapped, so authored colors reach the display unchanged.
float4 Unlit(Sampler2D gTextures[], Material material, float2 baseColorUV)
{
    float4 color = float4(material.metallicRoughness.baseColorFactor, 1.0);
    uint baseColorIdx = material.metallicRoughness.baseColorTextureIndex;
    if (hasTexture(baseColorIdx))
        color *= gTextures[baseColorIdx].Sample(baseColorUV);
    return color;
}

// clang-format on
