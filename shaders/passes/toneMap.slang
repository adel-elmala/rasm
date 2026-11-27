// clang-format off
#include "agx.slang"
#include "heat.slang"

// The tone-map pass: one full-screen triangle that maps the HDR scene color to display range with AgX, after mixing
// in the bloom and multiplying by the camera's exposure. The sRGB swapchain does the final encoding.

// Must match gfx::ToneMapPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct ToneMapPushConstants
{
    uint    source;         // bindless index of the HDR color
    float   exposure;       // linear multiplier: 2 is one stop brighter
    uint    bloom;          // bindless index of the bloom (addBloomPasses), INVALID_TEXTURE_INDEX without one
    float   bloomStrength;  // the bloom's share of the image, 0 to 1
};

static const uint INVALID_TEXTURE_INDEX = 0xFFFFFFFF;

[[vk::push_constant]]
ConstantBuffer<ToneMapPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    // Clip space is Y-up and uv v = 0 is the image's top row.
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float3 color = gTextures[pc.source].Sample(input.uv).rgb;
    // The bloom is the image blurred at every scale, with the image's energy, so mixing it in keeps the light's total.
    // It is half the size, so the bilinear lookup also scales it up.
    if (pc.bloom != INVALID_TEXTURE_INDEX)
        color = lerp(color, gTextures[pc.bloom].Sample(input.uv).rgb, pc.bloomStrength);
    return float4(agx(color * pc.exposure), 1.0);
}

// A debug view's display (Engine::setDebugView), without exposure, bloom or curve. fragDebug shows the values as
// they are: they are display values, so it decodes them for the sRGB attachment. fragDebugHeat shows a count
// (DebugView::OVERDRAW, LIGHT_COUNT) as a heat color (lib/heat.slang).

float3 decodeSrgb(float3 value)
{
    float3 v = saturate(value);
    return select(v <= 0.04045, v / 12.92, pow((v + 0.055) / 1.055, 2.4));
}

[shader("fragment")]
float4 fragDebug(VSOutput input) : SV_TARGET
{
    return float4(decodeSrgb(gTextures[pc.source].Sample(input.uv).rgb), 1.0);
}

// The count is rounded: a resolved or blended edge holds a share of each side, and the target's clear color is
// close to 0 (rasm::BACKGROUND_COLOR).
[shader("fragment")]
float4 fragDebugHeat(VSOutput input) : SV_TARGET
{
    return float4(decodeSrgb(getHeatColor(round(gTextures[pc.source].Sample(input.uv).r))), 1.0);
}

// clang-format on
