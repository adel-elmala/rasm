// clang-format off
#include "core.slang"

// The depth-only scene passes of a depth-only target (a shadow map): the draws of rasm.slang without shading.
// Opaque draws use vertMain and the empty fragMain; MASK and BLEND draws use vertMask and fragMask.

struct VSInput
{
    uint vertexID : SV_VertexID;
};

[shader("vertex")]
float4 vertMain(VSInput input) : SV_POSITION
{
    DrawInfo *draw          = pc.scene->drawInfoPtr + getDrawIndex();
    InstanceData *instance  = pc.scene->instancesPtr + draw->firstInstance;
    // SV_VertexID is the raw index-buffer value, as in rasm.slang.
    VertexFull vert         = getVertex(pc.scene, instance, draw, input.vertexID);
    return mul(pc.scene->projection, mul(pc.scene->view, mul(getWorldMatrix(pc.scene, instance), float4(vert.pos, 1.0))));
}

[shader("fragment")]
void fragMain()
{
}

struct MaskOutput
{
    float4 clipPosition     : SV_POSITION;
    float3 objectPosition   : POSITION;     // the hashed alpha test's noise lives on the object, so it does not crawl
    float  vertexAlpha      : COLOR;
    float2 uv0              : TEXCOORD0;
    float2 uv1              : TEXCOORD1;
    float2 uv2              : TEXCOORD2;
    nointerpolation uint materialIndex;
};

[shader("vertex")]
MaskOutput vertMask(VSInput input)
{
    DrawInfo *draw          = pc.scene->drawInfoPtr + getDrawIndex();
    InstanceData *instance  = pc.scene->instancesPtr + draw->firstInstance;
    VertexFull vert         = getVertex(pc.scene, instance, draw, input.vertexID);

    MaskOutput output       = {};
    output.clipPosition     = mul(pc.scene->projection, mul(pc.scene->view, mul(getWorldMatrix(pc.scene, instance), float4(vert.pos, 1.0))));
    output.objectPosition   = vert.pos;
    output.vertexAlpha      = getVertexColor(pc.scene, instance->geometryIndex, input.vertexID).a;
    output.uv0              = vert.uv[0];
    output.uv1              = vert.uv[1];
    output.uv2              = vert.uv[2];
    output.materialIndex    = instance->materialIndex;
    return output;
}

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

// A hash of an integer cell: three rounds of PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020).
uint3 hashCell(int3 cell)
{
    uint3 v = uint3(cell) * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float hashToUnit(float3 position)
{
    return float(hashCell(int3(floor(position))).x >> 8) / 16777216.0;
}

// Hashed alpha testing (Wyman and McGuire, 2017): a threshold in (0, 1] that is white noise over the object, at
// about one cell per texel of the map. Cells are fixed on the object, so the noise does not move from frame to frame.
// The two nearest power-of-two cell sizes are blended, and the blend's distribution is turned back into a uniform
// one, so the cells do not pop as the object moves nearer or farther.
float getHashedAlphaThreshold(float3 objectPosition)
{
    float maxDerivative = max(length(ddx(objectPosition)), length(ddy(objectPosition)));
    float pixelScale    = 1.0 / max(maxDerivative, 1e-8);
    float scaleLog      = log2(pixelScale);
    float2 scales       = float2(exp2(floor(scaleLog)), exp2(ceil(scaleLog)));
    float2 hashes       = float2(hashToUnit(scales.x * objectPosition), hashToUnit(scales.y * objectPosition));
    float blend         = frac(scaleLog);
    float x             = lerp(hashes.x, hashes.y, blend);
    float a             = max(min(blend, 1.0 - blend), 1e-6);
    float3 cases        = float3(x * x / (2.0 * a * (1.0 - a)), (x - 0.5 * a) / (1.0 - a), 1.0 - (1.0 - x) * (1.0 - x) / (2.0 * a * (1.0 - a)));
    float threshold     = x < 1.0 - a ? (x < a ? cases.x : cases.y) : cases.z;
    return clamp(threshold, 1e-6, 1.0);
}

// MASK keeps its cutoff in the map. BLEND keeps a share of the texels that follows its alpha, so the shadow filter's
// average gives a shadow as dense as the surface: faint glass casts a faint shadow, a leaf a full one.
[shader("fragment")]
void fragMask(MaskOutput input)
{
    MaterialData *model = pc.scene->materialsPtr + input.materialIndex;
    float alpha         = getBaseAlpha(gTextures, model, input.uv0, input.uv1, input.uv2, input.vertexAlpha);
    float threshold     = model->alphaMode == AlphaMode.BLEND ? getHashedAlphaThreshold(input.objectPosition) : model->alphaCutoff;
    if (alpha < threshold)
        discard;
}
// clang-format on
