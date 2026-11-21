// clang-format off
#pragma once

#include "core.slang"
#include "brdf.slang"
#include "environment.slang"
#include "lightClusters.slang"
#include "shadow.slang"

// Perturb the normal with a tangent-space normal map. Meshes with glTF TANGENTs (tangent.w = ±1) use the
// authored frame. Otherwise a UV-aligned frame is reconstructed from fragment derivatives, which supports
// mirrored UVs and the normal texture's selected UV set.
float3 getNormalFromMap(Sampler2D texture, float2 uv, float3 position, float3 normal, float4 tangent, float scale, bool normalXY)
{
    float3 N = normalize(normal);
    float3 tangentNormal = texture.Sample(uv).xyz * 2.0 - 1.0;
    // A two-channel map (BC5, EAC RG11) stores x and y of a unit normal; z is the rest of its length.
    if (normalXY)
        tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
    tangentNormal.xy *= scale;

    if (abs(tangent.w) > 0.5)
    {
        // Re-orthogonalize: interpolation leaves T slightly off the surface. The glTF spec defines
        // the bitangent as cross(N, T) * w.
        float3 T = tangent.xyz - N * dot(N, tangent.xyz);
        float authoredLength2 = dot(T, T);
        if (authoredLength2 > 1e-16)
        {
            T *= rsqrt(authoredLength2);
            float3 B = cross(N, T) * (tangent.w < 0.0 ? -1.0 : 1.0);
            return normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
        }
    }

    float3 dpdx = ddx(position);
    float3 dpdy = ddy(position);
    float2 duvdx = ddx(uv);
    float2 duvdy = ddy(uv);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    float3 T = (dpdx * duvdy.y - dpdy * duvdx.y);
    // Dividing by the UV determinant preserves handedness for mirrored UV islands.
    T *= determinant < 0.0 ? -1.0 : 1.0;
    T -= N * dot(N, T);
    float tLength2 = dot(T, T);
    if (abs(determinant) < 1e-10 || tLength2 < 1e-16)
        return N;
    T *= rsqrt(tLength2);
    // dP/dv: the direction in which v increases.
    float3 dpdv = (dpdy * duvdx.x - dpdx * duvdy.x) * (determinant < 0.0 ? -1.0 : 1.0);
    // glTF puts the UV origin at the image's top-left and normal maps store +Y = up in the image, so the
    // bitangent points toward decreasing v (-dP/dv), matching cross(N, T) * w for authored tangents.
    // Choosing B's sign against dP/dv, not from the UV determinant alone, keeps a flipped viewport
    // from flipping normal-map Y.
    float3 B = cross(N, T);
    B *= dot(B, dpdv) > 0.0 ? -1.0 : 1.0;
    // Explicit vector combination avoids confusing row/column matrix conventions.
    return normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
}

// Structure containing all the necessary information for PBR shading calculations
struct PBRInfo
{
    float3      vPosition;           // world position
    float3      vNormal;             // world normal
    float4      vTangent;            // world tangent, w = bitangent sign (0 when the mesh has no tangents)
    float3      cameraPosition;      // world camera position
    bool        backFace;            // the fragment is on the back of a double-sided surface
    float2      baseColorUV;         // UV coordinates for the base color texture
    float2      metallicRoughnessUV; // UV coordinates for the metallic-roughness texture
    float2      normalUV;            // UV coordinates for the normal map
    float2      occlusionUV;         // UV coordinates for the occlusion map
    float2      emissiveUV;          // UV coordinates for the emissive texture
    float4      vertexColor;         // COLOR_0, linear; white without one
    float2      pixel;               // SV_Position.xy, which picks the light cluster
    Material    material;            // material properties
    bool        normalXY;            // the normal map has x and y only (MATERIAL_NORMAL_XY)
    EnvironmentLight environment;    // the scene's environment; specular is INVALID_TEXTURE_INDEX without one
};

// The light one light reflects toward v: Cook-Torrance specular and Lambert diffuse. 0 where the light does not reach
// (outside its range or its cone) or comes from behind the surface. shadow is how much of the light reaches the point,
// 1 without a shadow.
float3 shadeLight(Light light, float shadow, float3 position, float3 n, float3 v, float NoV, float3 f0, float3 diffuseColor, float alpha)
{
    float3 l;
    float attenuation = shadow;

    if (light.type == LightType.DIRECTIONAL) {
        // Directional Light: direction points from the light towards the surface,
        // L points from the surface towards the light.
        l = normalize(-light.direction);
    }
    else {
        // Point and spot lights
        float3 lightDir         = light.position - position;
        float distanceSquared = max(dot(lightDir, lightDir), 1e-6);
        float distanceToLight = sqrt(distanceSquared);
        l = lightDir / distanceToLight;

        // Physical inverse-square law attenuation with clamping range
        attenuation *= 1.0 / distanceSquared;
        // A smooth window that reaches 0 at the range, so a light lights nothing beyond it and the light clusters can
        // leave it out there. A zero range means unbounded inverse-square falloff: a global light.
        if (light.range > 0.0)
            attenuation *= pow(max(1.0 - pow(distanceToLight / light.range, 4.0), 0.0), 2.0);
        // glTF KHR_lights_punctual: full light inside the inner cone, none outside the outer one, a smooth fade
        // between them.
        if (light.type == LightType.SPOT)
        {
            float cone = saturate(dot(normalize(light.direction), -l) * light.spotScale + light.spotOffset);
            attenuation *= cone * cone;
        }
    }

    float3 halfVector = v + l;
    float3 h = halfVector * rsqrt(max(dot(halfVector, halfVector), 1e-8));
    float  NoL = clamp(dot(n, l), 0.0, 1.0);
    float  NoH = clamp(dot(n, h), 0.0, 1.0);
    float  LoH = clamp(dot(l, h), 0.0, 1.0);

    if (NoL <= 0.0)
        return float3(0.0);

    // Evaluate Cook-Torrance Specular BRDF terms
    float  D = D_GGX(NoH, alpha);
    float3 F = F_Schlick(LoH, f0);
    float  V = V_SmithGGXCorrelated(NoV, NoL, alpha);

    float3 Fr = (D * V) * F;
    float3 Fd = (1.0 - F) * diffuseColor * Fd_Lambert();

    // Total surface reflection for this light source
    float3 radiance = light.color * light.intensity * attenuation * NoL;
    return (Fd + Fr) * radiance;
}

float4 shadePBR(Sampler2D gTextures[], SamplerCube gCubeTextures[], PBRInfo pbrInfo)
{
    float metallicFactor    = pbrInfo.material.metallicRoughness.metallicFactor;
    float roughnessFactor   = pbrInfo.material.metallicRoughness.roughnessFactor;
    float3 emissiveFactor   = pbrInfo.material.emissive.emissiveFactor;

    // 1. Sample textures (when the material has one) and combine with the factors.
    // glTF defaults keep the render correct when a texture is absent: baseColor = factor only,
    // roughness/metallic = factors, occlusion = 1, emissive = 0, geometric normal.

    float4 baseColorTex = sampleBaseColor(gTextures, pbrInfo.material.metallicRoughness, pbrInfo.baseColorUV, pbrInfo.vertexColor);
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
        n = getNormalFromMap(gTextures[normalIdx], pbrInfo.normalUV, pbrInfo.vPosition, pbrInfo.vNormal, pbrInfo.vTangent, pbrInfo.material.normal.scale, pbrInfo.normalXY);
    n = normalize(n);
    // glTF: a double-sided back face is lit with the reversed normal (the front's T, B and N all negated).
    if (pbrInfo.backFace)
        n = -n;

    float3 v    = normalize(pbrInfo.cameraPosition - pbrInfo.vPosition);
    float  NoV  = max(dot(n, v), 1e-5);

    // 3. Setup reflectance at normal incidence (f0)
    // Dielectrics have a constant f0 of ~0.04 (4% reflectance), metals use baseColor
    float3 dielectricF0 = float3(0.04);
    float3 f0           = lerp(dielectricF0, baseColor, metallic);
    float3 diffuseColor = (1.0 - metallic) * baseColor;

    // Roughness remapping (perceptually linear roughness squared)
    float alpha = roughness * roughness;

    float3 totalDirectLighting = float3(0.0);
    float3 position = pbrInfo.vPosition;
    SceneData *scene = pc.scene;

    // 4. The lights, in three loops. A loop's light index is the same for the whole draw where it can be, so the
    // shadow maps' texture indices stay uniform.
    // a. The view's shadowed lights. The shadow is read only where the light reaches the surface (P12).
    float3 shadowNormal = pbrInfo.backFace ? -pbrInfo.vNormal : pbrInfo.vNormal;
    for (uint s = 0; s < scene->shadowedLightCount; ++s)
    {
        ShadowedLight shadowed = scene->shadowedLights[s];
        Light light = scene->lightsPtr[shadowed.light];
        float3 lit = shadeLight(light, 1.0, position, n, v, NoV, f0, diffuseColor, alpha);
        if (all(lit == float3(0.0)))
            continue;
        totalDirectLighting += lit * sampleShadow(gTextures, gCubeTextures, scene, light, shadowed, position, shadowNormal);
    }
    // b. The global lights (directional, or without a range) reach every pixel.
    for (uint i = 0; i < scene->globalLightCount; ++i)
    {
        uint row = scene->sceneLightsPtr[i];
        if (!isShadowedLight(scene, row))
            totalDirectLighting += shadeLight(scene->lightsPtr[row], 1.0, position, n, v, NoV, f0, diffuseColor, alpha);
    }
    // c. The local lights: those of the pixel's cluster, or every one when the view has no clusters.
    uint32_t *clusters = scene->clustersPtr;
    if (clusters != nullptr)
    {
        float viewDepth = -mul(scene->view, float4(position, 1.0)).z;
        int cluster = getCluster(scene, pbrInfo.pixel, viewDepth);
        uint count = cluster < 0 ? 0 : min(clusters[CLUSTER_COUNTS + cluster], MAX_CLUSTER_LIGHTS);
        uint list = CLUSTER_LISTS + uint(max(cluster, 0)) * MAX_CLUSTER_LIGHTS;
        for (uint i = 0; i < count; ++i)
            totalDirectLighting += shadeLight(scene->lightsPtr[clusters[list + i]], 1.0, position, n, v, NoV, f0, diffuseColor, alpha);
    }
    else
    {
        for (uint i = 0; i < scene->localLightCount; ++i)
        {
            uint row = scene->sceneLightsPtr[scene->globalLightCount + i];
            if (!isShadowedLight(scene, row))
                totalDirectLighting += shadeLight(scene->lightsPtr[row], 1.0, position, n, v, NoV, f0, diffuseColor, alpha);
        }
    }

    // 5. The light from around the scene: the environment's (image-based lighting), or without one a small constant.
    // Neither is shadowed.
    float3 ambient = float3(0.03) * diffuseColor * ao;
    if (hasTexture(pbrInfo.environment.specular))
        ambient = getEnvironmentLight(gCubeTextures, gTextures, pbrInfo.environment, n, v, NoV, roughness, f0, diffuseColor) * ao;

    // 6. Final color composition. Radiance goes out unbounded, into the HDR target; the tone-map pass brings it to
    // display range.
    float3 finalColor = ambient + totalDirectLighting + emissive;

    return float4(finalColor, baseColorTex.a);
}


// clang-format on