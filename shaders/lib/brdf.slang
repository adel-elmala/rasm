// clang-format off
#pragma once

// The BRDF terms, with the names they have in the literature. They read no engine data, so the environment bake uses
// them too.

// Constants
static const float  PI                  = 3.14159265359;

// Normal distribution function (GGX / Trowbridge-Reitz)
float D_GGX(float NoH, float roughness)
{
    float a = NoH * roughness;
    float k = roughness / (1.0 - NoH * NoH + a * a);
    return k * k * (1.0 / PI);
}

// Fresnel term (Schlick's approximation)
float3 F_Schlick(float u, float3 f0)
{
    float f = pow(1.0 - u, 5.0);
    return f + f0 * (1.0 - f);
}

// Height-correlated Smith-GGX visibility (shadowing/masking)
float V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    float a2 = roughness * roughness;
    float GGXL = NoV * sqrt((-NoL * a2 + NoL) * NoL + a2);
    float GGXV = NoL * sqrt((-NoV * a2 + NoV) * NoV + a2);
    return 0.5 / max(GGXV + GGXL, 1e-8);
}

// Lambertian diffuse BRDF
float Fd_Lambert()
{
    return 1.0 / PI;
}

// clang-format on
