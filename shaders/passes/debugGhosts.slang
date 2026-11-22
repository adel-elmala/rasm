// clang-format off
// The culling view's ghosts (DebugView::CULLING): after the view's last pass, a box around the bounding sphere of every
// draw of the scene's list, one instance per draw, without a depth test and added onto the target, so a culled draw
// shows where it is through what is in front of it. The box is the sphere culling tested, not the mesh: every draw's
// LOD 0 would cost as much as the scene's whole triangle count. The vertex shader drops the draws the frame drew. A
// draw in the frustum that was not drawn was occluded (red); the others were outside the frustum (blue). With
// occlusion culling, the drawn ones are those the visibility buffer saw; with frustum culling only, those cull.slang
// gave a slot. While culling is frozen, the frustum and the visibility are the frozen ones.

#include "core.slang"
#include "frustumCulling.slang"

static const uint   CULLED          = 0xFFFFFFFF;   // Must match CULLED in cull.slang.
static const float3 OCCLUDED_COLOR  = float3(0.35, 0.0, 0.0);
static const float3 OUTSIDE_COLOR   = float3(0.0, 0.0, 0.35);
// A box's 12 triangles, counter-clockwise seen from outside. Corner c is at (c & 1, c >> 1 & 1, c >> 2 & 1), from -1 to 1.
static const uint   BOX_CORNERS[36] = {5, 1, 3, 5, 3, 7,   0, 4, 6, 0, 6, 2,   6, 7, 3, 6, 3, 2,
                                       0, 1, 5, 0, 5, 4,   4, 5, 7, 4, 7, 6,   1, 0, 2, 1, 2, 3};

struct VSOutput
{
    float4 clipPosition         : SV_POSITION;
    nointerpolation float3 color : COLOR;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint drawIndex : SV_InstanceID)
{
    SceneData *scene        = pc.scene;
    DrawInfo draw           = scene->allDrawsPtr[drawIndex];
    Sphere sphere           = getWorldSphere(scene, draw);
    bool inFrustum          = isInFrustum(scene, sphere);
    bool drawn              = scene->hizLevelCount > 0 ? inFrustum && scene->visibilityPtr[draw.firstInstance] != 0 : scene->cullSlotsPtr[drawIndex] != CULLED;

    VSOutput output         = {};
    // Behind the near plane (z < 0), so the drawn draws' triangles are clipped away.
    output.clipPosition     = float4(0.0, 0.0, -1.0, 1.0);
    if (drawn)
        return output;
    uint corner             = BOX_CORNERS[vertexID];
    float3 side             = float3(float(corner & 1), float((corner >> 1) & 1), float((corner >> 2) & 1)) * 2.0 - 1.0;
    float3 position         = sphere.center + side * sphere.radius;
    output.clipPosition     = mul(scene->projection, mul(scene->view, float4(position, 1.0)));
    output.color            = inFrustum ? OCCLUDED_COLOR : OUTSIDE_COLOR;
    return output;
}

// A ghost covers every other band of 4 pixels, on a diagonal, so the surface under it still shows its own color
// between the bands.
bool isGhostGap(float2 pixel)
{
    uint2 p = uint2(pixel);
    return ((p.x + p.y) & 4) != 0;
}

#if defined(RASM_MOTION_VECTORS)
// The pipeline adds onto both colors, so a ghost leaves the motion as it is.
struct FragmentOutput
{
    float4 color  : SV_TARGET0;
    float4 motion : SV_TARGET1;
};

[shader("fragment")]
FragmentOutput fragMain(VSOutput input)
{
    if (isGhostGap(input.clipPosition.xy))
        discard;
    FragmentOutput output;
    output.color    = float4(input.color, 0.0);
    output.motion   = float4(0.0);
    return output;
}
#else
[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    if (isGhostGap(input.clipPosition.xy))
        discard;
    return float4(input.color, 0.0);
}
#endif
// clang-format on
