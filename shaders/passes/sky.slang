// clang-format off
// The sky: one triangle over the whole target at the far plane (depth 0 in reversed-Z), drawn after the scene in the
// view's last draw pass. The depth test (GREATER_OR_EQUAL) passes only where nothing was drawn, and there it shows the
// environment's cube in the pixel's view direction.

#include "core.slang"
#include "environment.slang"

[[vk::binding(0, 0)]]
SamplerCube gCubeTextures[];

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    float2 uv       = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput output;
    // Clip space is Y-up, so the top of the target is y = 1.
    output.ndc      = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    output.pos      = float4(output.ndc, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float4x4 projection = pc.scene->projection;
    // The view-space ray through the pixel. An orthographic camera looks the same way from every pixel.
    float3 viewRay      = float3(0.0, 0.0, -1.0);
    if (projection[3][3] == 0.0)
        viewRay         = float3(input.ndc.x / projection[0][0], input.ndc.y / projection[1][1], -1.0);
    // The view matrix's 3x3 is a rotation, so its transpose turns view space back into world space.
    float3 direction    = normalize(mul(transpose((float3x3)pc.scene->view), viewRay));
    float3 color        = gCubeTextures[pc.scene->environmentSky].SampleLevel(toEnvironment(direction, pc.scene->environmentRotation), 0.0).rgb;
    return float4(color * pc.scene->environmentIntensity, 1.0);
}

// clang-format on
