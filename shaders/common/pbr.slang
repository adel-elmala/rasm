// clang-format off
#pragma once

//  Types
enum LightType : uint32_t { Directional , Point };

// Must match rasm::gfx::Light exactly (scalar layout): the lights array sits between numLights and
// the buffer device addresses in SceneData, so any size drift shifts vertsPtr/drawInfoPtr/
// modeldataPtr and the shader dereferences a garbage address (device lost).
struct Light
{
    float3      position;
    float       intensity;
    float3      color;
    float       range;      // Only used for point lights
    float3      direction;  // Only used for directional lights
    LightType   type;
};

struct MetallicRoughnessMaterial
{
    float3      baseColorFactor;
    uint32_t    baseColorTextureIndex;
    uint32_t    baseColorTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    uint32_t    metallicRoughnessTextureIndex;
    uint32_t    metallicRoughtnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       metallicFactor;
    float       roughnessFactor;
};

struct NormalMaterial
{
    uint32_t    normalTextureIndex;
    uint32_t    normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       scale;
};

struct OcclusionMaterial
{
    uint32_t    occlusionTextureIndex;
    uint32_t    occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       strength;
};

struct EmissiveMaterial
{
    uint32_t    emissiveTextureIndex;
    uint32_t    emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float3      emissiveFactor;
};

struct Material
{
    MetallicRoughnessMaterial   metallicRoughness;
    NormalMaterial              normal;
    OcclusionMaterial           occlusion;
    EmissiveMaterial            emissive;
};

// Constants
static const float  PI                  = 3.14159265359;
static const uint   MAX_LIGHTS          = 8;
static const uint   INVALID_TEXTURE_INDEX = 0xFFFFFFFF;

bool hasTexture(uint textureIndex)
{
    return textureIndex != INVALID_TEXTURE_INDEX;
}

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
    return 0.5 / (GGXV + GGXL);
}

// Lambertian diffuse BRDF
float Fd_Lambert()
{
    return 1.0 / PI;
}

float3 getNormalFromMap(Sampler2D uNormalTexture, float2 uv, float3 normal, float3 tangent)
{
    float3 tangentNormal = (uNormalTexture.Sample(uv).xyz) * 2.0 - 1.0;

    float3 N = normalize(normal);
    float3 T = normalize(tangent);

    float3 B = cross(N, T);
    if (dot(B, B) < 1e-8)
    {
        // The vertex stage currently passes the normal as the tangent (meshes have no tangents
        // yet), which makes cross(N, T) zero and the TBN NaN. Build a valid orthonormal basis
        // from an arbitrary helper axis instead.
        float3 helper = abs(N.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        T = normalize(cross(helper, N));
        B = cross(N, T);
    }
    else
    {
        T = normalize(T);
        B = normalize(B);
    }

    float3x3 TBN = float3x3(T, B, N);
    return normalize(mul(TBN, tangentNormal));
}

// Structure containing all the necessary information for PBR shading calculations
struct PBRInfo
{
    float3      vPosition;           // world position
    float3      vNormal;             // world normal
    float3      vTangent;            // world tangent
    float3      cameraPosition;      // world camera position
    float2      baseColorUV;         // UV coordinates for the base color texture
    float2      metallicRoughnessUV; // UV coordinates for the metallic-roughness texture
    float2      normalUV;            // UV coordinates for the normal map
    float2      occlusionUV;         // UV coordinates for the occlusion map
    float2      emissiveUV;          // UV coordinates for the emissive texture
    uint        numLights;           // number of lights
    Light       lights[MAX_LIGHTS];  // array of lights
    Material    material;            // material properties
};

float4 PBR(Sampler2D gTextures[], PBRInfo pbrInfo)
{
    float3 baseColorFactor  = pbrInfo.material.metallicRoughness.baseColorFactor;
    float metallicFactor    = pbrInfo.material.metallicRoughness.metallicFactor;
    float roughnessFactor   = pbrInfo.material.metallicRoughness.roughnessFactor;
    float3 emissiveFactor   = pbrInfo.material.emissive.emissiveFactor;

    // 1. Sample textures (when the material has one) and combine with the factors.
    // glTF defaults keep the render correct when a texture is absent: baseColor = factor only,
    // roughness/metallic = factors, occlusion = 1, emissive = 0, geometric normal.

    uint baseColorIdx = pbrInfo.material.metallicRoughness.baseColorTextureIndex;
    float4 baseColorTex = float4(baseColorFactor, 1.0);
    if (hasTexture(baseColorIdx))
        baseColorTex = gTextures[baseColorIdx].Sample(pbrInfo.baseColorUV) * float4(baseColorFactor, 1.0);
    float3 baseColor = baseColorTex.rgb;

    // glTF packs roughness in the G channel (green) and metallic in the B channel (blue)
    float roughness = clamp(roughnessFactor, 0.045, 1.0); // Clamped to prevent artifacts
    float metallic  = clamp(metallicFactor, 0.0, 1.0);
    uint mrIdx = pbrInfo.material.metallicRoughness.metallicRoughnessTextureIndex;
    if (hasTexture(mrIdx))
    {
        float4 mrTex  = gTextures[mrIdx].Sample(pbrInfo.metallicRoughnessUV);
        roughness = clamp(mrTex.g * roughnessFactor, 0.045, 1.0);
        metallic  = clamp(mrTex.b * metallicFactor, 0.0, 1.0);
    }

    // glTF: emissive = factor * texture, and the factor alone still glows when no texture exists
    float3 emissive = emissiveFactor;
    uint emissiveIdx = pbrInfo.material.emissive.emissiveTextureIndex;
    if (hasTexture(emissiveIdx))
        emissive *= gTextures[emissiveIdx].Sample(pbrInfo.emissiveUV).rgb;

    float ao = 1.0;
    uint occlusionIdx = pbrInfo.material.occlusion.occlusionTextureIndex;
    if (hasTexture(occlusionIdx))
    {
        float occlusion = gTextures[occlusionIdx].Sample(pbrInfo.occlusionUV).r;
        ao = lerp(1.0, occlusion, pbrInfo.material.occlusion.strength);
    }

    // 2. Setup vectors
    float3 n = pbrInfo.vNormal;
    uint normalIdx = pbrInfo.material.normal.normalTextureIndex;
    if (hasTexture(normalIdx))
        n = getNormalFromMap(gTextures[normalIdx], pbrInfo.normalUV, pbrInfo.vNormal, pbrInfo.vTangent);
    n = normalize(n);

    float3 v    = normalize(pbrInfo.cameraPosition - pbrInfo.vPosition);
    float  NoV  = abs(dot(n, v)) + 1e-5;

    // 3. Setup reflectance at normal incidence (f0)
    // Dielectrics have a constant f0 of ~0.04 (4% reflectance), metals use baseColor
    float3 dielectricF0 = float3(0.04);
    float3 f0           = lerp(dielectricF0, baseColor, metallic);
    float3 diffuseColor = (1.0 - metallic) * baseColor;

    // Roughness remapping (perceptually linear roughness squared)
    float alpha = roughness * roughness;

    float3 totalDirectLighting = float3(0.0);

    // 4. Accumulate light contributions (Multiple Lights Loop)
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= pbrInfo.numLights)
            break;

        Light light = pbrInfo.lights[i];

        float3 l;
        float attenuation = 1.0;

        if (light.type == LightType.Directional) {
            // Directional Light: direction points from the light towards the surface,
            // L points from the surface towards the light.
            l = normalize(-light.direction);
        }
        else {
            // Point Light
            float3 lightDir         = light.position - pbrInfo.vPosition;
            float  distanceToLight   = length(lightDir);

            l = normalize(lightDir);

            // Physical inverse-square law attenuation with clamping range
            attenuation = 1.0 / (distanceToLight * distanceToLight);
            // Optional smooth falloff window:
            attenuation *= pow(max(1.0 - pow(distanceToLight / light.range, 4.0), 0.0), 2.0);
        }

        float3 h   = normalize(v + l);
        float  NoL = clamp(dot(n, l), 0.0, 1.0);
        float  NoH = clamp(dot(n, h), 0.0, 1.0);
        float  LoH = clamp(dot(l, h), 0.0, 1.0);

        if (NoL > 0.0 || NoV > 0.0) {
            // Evaluate Cook-Torrance Specular BRDF terms
            float  D = D_GGX(NoH, alpha);
            float3 F = F_Schlick(LoH, f0);
            float  V = V_SmithGGXCorrelated(NoV, NoL, alpha);

            float3 Fr = (D * V) * F;
            float3 Fd = diffuseColor * Fd_Lambert();

            // Total surface reflection for this light source
            float3 radiance = light.color * light.intensity * attenuation * NoL;
            totalDirectLighting += (Fd + Fr) * radiance;
        }
    }

    // 5. Ambient / Environment lighting approximation
    float3 ambient = float3(0.03) * baseColor * ao; // Simple ambient baseline

    // 6. Final color composition
    float3 finalColor = ambient + totalDirectLighting + emissive;

    // Simple tone mapping (ACES / Gamma correction)
    finalColor = finalColor / (finalColor + float3(1.0)); // Reinhard tone mapping
    finalColor = pow(finalColor, float3(1.0 / 2.2));      // Gamma correction

    return float4(finalColor, baseColorTex.a);
}


// clang-format on