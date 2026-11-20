// clang-format off
// The tone-map pass: one full-screen triangle that maps the HDR scene color to display range with AgX, after
// multiplying it by the camera's exposure. The sRGB swapchain does the final encoding.

// Must match gfx::ToneMapPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct ToneMapPushConstants
{
    uint    source;     // bindless index of the HDR color
    float   exposure;   // linear multiplier: 2 is one stop brighter
};

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

// AgX as in Blender 4, in the minimal form of Benjamin Wrensch ("Minimal AgX implementation", 2023): the inset
// matrix pulls saturated colors toward white before the curve, so bright lights go white instead of changing hue.
// Each row sums to 1, so grays stay gray.
static const float3x3 AGX_INSET  = float3x3(0.842479062253094,   0.0784335999999992,  0.0792237451477643,
                                             0.0423282422610123,  0.878468636469772,   0.0791661274605434,
                                             0.0423756549057051,  0.0784336,           0.879142973793104);
static const float3x3 AGX_OUTSET = float3x3(1.19687900512017,   -0.0980208811401368, -0.0990297440797205,
                                            -0.0528968517574562,  1.15190312990417,   -0.0989611768448433,
                                            -0.0529716355144438, -0.0980434501171241,  1.15107367264116);
// The curve covers 16.5 stops, from 2^-12.47 to 2^4.03 (about 0.00018 to 16.3): 10 below middle gray (0.18), 6.5 above.
static const float    AGX_MIN_EV = -12.47393;
static const float    AGX_MAX_EV = 4.026069;

// A polynomial fit of AgX's sigmoid, on log2 values scaled to [0, 1].
float3 agxContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Linear scene color in, linear display color in [0, 1] out.
float3 agx(float3 color)
{
    float3 v = mul(AGX_INSET, max(color, 0.0));
    v = clamp(log2(max(v, 1e-10)), AGX_MIN_EV, AGX_MAX_EV);
    v = agxContrast((v - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV));
    v = mul(AGX_OUTSET, v);
    // The curve's output is display-encoded with a 2.2 gamma. Decode it, since the attachment encodes to sRGB.
    return pow(saturate(v), 2.2);
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float3 color = gTextures[pc.source].Sample(input.uv).rgb;
    return float4(agx(color * pc.exposure), 1.0);
}

// clang-format on
