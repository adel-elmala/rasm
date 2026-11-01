// clang-format off
#pragma once

#include "core.slang"

// Base color only (KHR_materials_unlit). Not tone mapped, so authored colors reach the display unchanged.
float4 Unlit(Sampler2D gTextures[], MetallicRoughnessMaterial material, float2 baseColorUV)
{
    return sampleBaseColor(gTextures, material, baseColorUV);
}

// clang-format on
