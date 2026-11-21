// clang-format off
#include "animationTypes.slang"

// The Skin pass: once per frame, after the Animate pass and before any view. A group of the dispatch is a row of the
// animated entity table (x) and 64 of that entity's skinned vertices (y). Each thread mixes its vertex's 4 joint
// matrices by their weights and writes the posed vertex, with the base vertex's layout, to this slot's skinned
// vertices. Every view then reads those, so a vertex is skinned once per frame, however many views draw it.

static const uint SKIN_GROUP_SIZE = 64;

[[vk::push_constant]]
ConstantBuffer<AnimatePushConstants> pc;

float3x4 loadJoint(AnimationFrame *frame, uint joint)
{
    float4 *rows = frame->joints + joint * 3;
    float3x4 m;
    m[0] = rows[0];
    m[1] = rows[1];
    m[2] = rows[2];
    return m;
}

// transpose(inverse(m)): the cofactors over the determinant, as the Animate pass's normal matrix.
float3x3 getNormalMatrix(float3x3 m)
{
    float3 c0 = cross(m[1], m[2]);
    float3 c1 = cross(m[2], m[0]);
    float3 c2 = cross(m[0], m[1]);
    float determinant = dot(m[0], c0);
    float scale = abs(determinant) > 1e-30 ? 1.0 / determinant : 1.0;
    return float3x3(c0 * scale, c1 * scale, c2 * scale);
}

[shader("compute")]
[numthreads(SKIN_GROUP_SIZE, 1, 1)]
void skinMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    AnimationFrame *frame = pc.frame;
    if (groupId.x >= frame->entityCount)
        return;
    AnimatedEntity entity = frame->entities[groupId.x];
    if (entity.clip < 0)
        return;
    AnimationMeshData *mesh = entity.mesh;
    uint vertex = groupId.y * SKIN_GROUP_SIZE + threadId.x;
    if (vertex >= mesh->skinnedVertexCount)
        return;

    // The entity's skinned vertices are its skinned sub-meshes' one after another.
    for (uint k = 0; k < mesh->subMeshCount; ++k)
    {
        AnimationSubMeshData subMesh = mesh->subMeshes[k];
        if (subMesh.jointCount == 0 || vertex < subMesh.firstSkinnedVertex || vertex >= subMesh.firstSkinnedVertex + subMesh.vertexCount)
            continue;

        uint local = vertex - subMesh.firstSkinnedVertex;
        GeometryData *geometry = frame->geometry + subMesh.geometryIndex;
        VertexFull source = frame->vertices[uint(geometry->vertexOffset) + local];
        SkinVertex skin = frame->skins[uint(geometry->skinOffset) + local];
        uint4 joints = uint4(skin.joints.x & 0xFFFF, skin.joints.x >> 16, skin.joints.y & 0xFFFF, skin.joints.y >> 16);
        float4 weights = float4(skin.weights.x & 0xFFFF, skin.weights.x >> 16, skin.weights.y & 0xFFFF, skin.weights.y >> 16) / 65535.0;

        uint first = entity.firstJoint + subMesh.firstJoint;
        float3x4 m = weights.x * loadJoint(frame, first + joints.x) + weights.y * loadJoint(frame, first + joints.y) +
                     weights.z * loadJoint(frame, first + joints.z) + weights.w * loadJoint(frame, first + joints.w);
        float3x3 m3 = float3x3(m[0].xyz, m[1].xyz, m[2].xyz);

        VertexFull posed = source;
        posed.pos = mul(m, float4(source.pos, 1.0));
        posed.normal = mul(getNormalMatrix(m3), source.normal);
        // A mirroring mix (a joint scaled through 0) turns the bitangent around, as the vertex shader does for a
        // mirroring entity.
        posed.tangent = float4(mul(m3, source.tangent.xyz), determinant(m3) < 0.0 ? -source.tangent.w : source.tangent.w);
        frame->skinnedVertices[entity.firstSkinnedVertex + vertex] = posed;
        return;
    }
}
// clang-format on
