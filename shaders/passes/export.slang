// clang-format off
#include "agx.slang"

// The Export pass (src/renderer/export.cpp): one sample of one tile of an export. It reads the view's resolved HDR
// color and its bloom, maps them as the tone-map pass does (AgX, or the linear HDR color without it), and adds the
// result, premultiplied by its alpha, times 1 / samples to the tile's sum. On the tile's last sample it writes the sum,
// encoded, into the output buffer the CPU reads: the kept rect only, without the guard band around it.

static const uint   EXPORT_GROUP_SIZE       = 8;            // Must match EXPORT_GROUP_SIZE in src/renderer/export.cpp.
static const uint   INVALID_TEXTURE_INDEX   = 0xFFFFFFFF;
// Must match gfx::EXPORT_* in src/renderer/uberMaterialTypes.h.
static const uint   EXPORT_FLAG_FIRST       = 1u << 0;
static const uint   EXPORT_FLAG_LAST        = 1u << 1;
static const uint   EXPORT_FLAG_TONE_MAP    = 1u << 2;
static const uint   EXPORT_FLAG_TRANSPARENT = 1u << 3;
static const uint   EXPORT_OUTPUT_SRGB8     = 0;
static const uint   EXPORT_OUTPUT_SRGB16    = 1;
static const uint   EXPORT_OUTPUT_HALF      = 2;
static const uint   EXPORT_OUTPUT_FLOAT     = 3;
// Below this alpha a pixel is empty: its straight color is 0, not a color divided by almost nothing.
static const float  EXPORT_MIN_ALPHA        = 1e-6;

// Must match gfx::ExportParams; validateRasmStructs checks it by name.
struct ExportParams
{
    float4* accumPtr;       // the tile's sum, a float4 per kept pixel
    uint*   outputPtr;      // the tile's pixels, EXPORT_OUTPUT_* encoded
    uint    source;         // bindless: the resolved HDR color
    uint    bloom;          // bindless: the bloom, INVALID_TEXTURE_INDEX without one
    float   exposure;
    float   bloomStrength;
    uint    offsetX;        // the kept rect's corner in the source
    uint    offsetY;
    uint    width;          // the kept rect's size
    uint    height;
    float   weight;         // 1 / samples
    uint    flags;
    uint    output;
    uint    padding;
};

struct ExportPushConstants
{
    ExportParams*   params;
    uint            padding[2];
};

[[vk::push_constant]]
ConstantBuffer<ExportPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

float3 encodeSrgb(float3 value)
{
    float3 v = saturate(value);
    return select(v <= 0.0031308, v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

// The straight color of a premultiplied one, 0 where nothing covers the pixel.
float3 unpremultiply(float4 value)
{
    return value.a > EXPORT_MIN_ALPHA ? value.rgb / value.a : float3(0.0);
}

uint packUnorm8(float4 value)
{
    uint4 v = uint4(round(saturate(value) * 255.0));
    return v.r | (v.g << 8) | (v.b << 16) | (v.a << 24);
}

uint2 packUnorm16(float4 value)
{
    uint4 v = uint4(round(saturate(value) * 65535.0));
    return uint2(v.r | (v.g << 16), v.b | (v.a << 16));
}

void writeOutput(ExportParams *params, uint index, float4 sum)
{
    uint *output = params->outputPtr;
    if (params->output == EXPORT_OUTPUT_SRGB8)
        output[index] = packUnorm8(float4(encodeSrgb(unpremultiply(sum)), sum.a));
    else if (params->output == EXPORT_OUTPUT_SRGB16)
    {
        uint2 packed = packUnorm16(float4(encodeSrgb(unpremultiply(sum)), sum.a));
        output[index * 2] = packed.x;
        output[index * 2 + 1] = packed.y;
    }
    else if (params->output == EXPORT_OUTPUT_HALF)
    {
        output[index * 2] = packHalf2x16(sum.xy);
        output[index * 2 + 1] = packHalf2x16(sum.zw);
    }
    else
    {
        output[index * 4] = asuint(sum.x);
        output[index * 4 + 1] = asuint(sum.y);
        output[index * 4 + 2] = asuint(sum.z);
        output[index * 4 + 3] = asuint(sum.w);
    }
}

// With a transparent background the scene's alpha is its coverage and its color is premultiplied by it: the color is
// mapped straight and premultiplied again, so an edge keeps its surface's color, and the bloom adds only where
// something covers the pixel. Without one every pixel is covered.
[shader("compute")]
[numthreads(EXPORT_GROUP_SIZE, EXPORT_GROUP_SIZE, 1)]
void exportMain(uint3 threadId : SV_DispatchThreadID)
{
    ExportParams *params = pc.params;
    if (threadId.x >= params->width || threadId.y >= params->height)
        return;
    int2 pixel          = int2(params->offsetX + threadId.x, params->offsetY + threadId.y);
    float4 hdr          = gTextures[params->source].Load(int3(pixel, 0));
    bool transparent    = (params->flags & EXPORT_FLAG_TRANSPARENT) != 0;
    float alpha         = transparent ? saturate(hdr.a) : 1.0;
    float3 color        = transparent ? unpremultiply(float4(hdr.rgb, alpha)) : hdr.rgb;
    // The tone-map pass samples the half-size bloom at the pixel's centre in the whole target, as here.
    if (params->bloom != INVALID_TEXTURE_INDEX)
    {
        uint width;
        uint height;
        gTextures[params->source].GetDimensions(width, height);
        float2 uv       = (float2(pixel) + 0.5) / float2(width, height);
        color           = lerp(color, gTextures[params->bloom].SampleLevel(uv, 0.0).rgb, params->bloomStrength);
    }
    color              *= params->exposure;
    if ((params->flags & EXPORT_FLAG_TONE_MAP) != 0)
        color           = agx(color);
    float4 value        = float4(color * alpha, alpha) * params->weight;

    uint index          = threadId.y * params->width + threadId.x;
    float4 sum          = (params->flags & EXPORT_FLAG_FIRST) != 0 ? value : params->accumPtr[index] + value;
    if ((params->flags & EXPORT_FLAG_LAST) != 0)
        writeOutput(params, index, sum);
    else
        params->accumPtr[index] = sum;
}
// clang-format on
