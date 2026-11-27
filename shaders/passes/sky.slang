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
    float4                  pos     : SV_POSITION;
    float2                  ndc     : TEXCOORD0;
    nointerpolation float2  window  : TEXCOORD1;    // the projection's offset of its centre without TAA's jitter: an export tile's window, else 0
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    float2 uv       = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput output;
    // Clip space is Y-up, so the top of the target is y = 1.
    output.ndc      = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    output.pos      = float4(output.ndc, 0.0, 1.0);
    // projection[i][2] is -jitter plus the window: per triangle, not per pixel.
    output.window   = float2(pc.scene->projection[0][2], pc.scene->projection[1][2]) + pc.scene->jitter;
    return output;
}

#if defined(RASM_MOTION_VECTORS)
// With a motion target, the sky's motion too: it is infinitely far, so only the camera's turn moves it.
struct FragmentOutput
{
    float4 color  : SV_TARGET0;
    float4 motion : SV_TARGET1;
};
#endif

[shader("fragment")]
#if defined(RASM_MOTION_VECTORS)
FragmentOutput fragMain(VSOutput input)
#else
float4 fragMain(VSOutput input) : SV_TARGET
#endif
{
    float4x4 projection = pc.scene->projection;
    // The view-space ray through the pixel. An orthographic camera looks the same way from every pixel. The ray
    // takes out the projection's TAA jitter (jitter), so the sky moves with the scene, and follows the rest of its
    // centre's offset (projection[i][2] is -jitter plus an export tile's off-center window).
    float3 viewRay      = float3(0.0, 0.0, -1.0);
    float2 ndc          = input.ndc - pc.scene->jitter;
    if (projection[3][3] == 0.0)
        viewRay         = float3((ndc.x + input.window.x) / projection[0][0], (ndc.y + input.window.y) / projection[1][1], -1.0);
    // The view matrix's 3x3 is a rotation, so its transpose turns view space back into world space.
    float3 direction    = normalize(mul(transpose((float3x3)pc.scene->view), viewRay));
    float3 color        = gCubeTextures[pc.scene->environmentSky].SampleLevel(toEnvironment(direction, pc.scene->environmentRotation, pc.scene->environmentOrientation), 0.0).rgb;
    float4 sky          = float4(color * pc.scene->environmentIntensity, 1.0);
#if defined(RASM_MOTION_VECTORS)
    // A direction has w = 0, so the camera's move drops out and only its turn moves the sky. An orthographic camera
    // shows one direction at every pixel, so its sky has no motion.
    FragmentOutput output;
    output.color        = sky;
    output.motion       = float4(0.0);
    if (projection[3][3] == 0.0)
    {
        float4 previousClip = mul(pc.scene->previousViewProjection, float4(direction, 0.0));
        output.motion   = float4(getMotion(float4(ndc, 0.0, 1.0), previousClip), 0.0, 0.0);
    }
    return output;
#else
    return sky;
#endif
}

// clang-format on
