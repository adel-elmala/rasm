// clang-format off
#include "core.slang"

// The depth-only scene pass of a depth-only target (a shadow map): the draws of rasm.slang without shading.

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
    VertexFull vert         = pc.scene->vertsPtr[input.vertexID + draw->vertexOffset];
    return mul(pc.scene->projection, mul(pc.scene->view, mul(instance->worldMatrix, float4(vert.pos, 1.0))));
}

[shader("fragment")]
void fragMain()
{
}
// clang-format on
