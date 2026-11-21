// clang-format off
#include "animationTypes.slang"

// The Skin pass: once per frame, after the Animate pass and before any view. A group of the dispatch is a row of the
// animated entity table (x) and 64 of that entity's skinned vertices (y), all of one sub-mesh: each posed sub-mesh
// starts on a multiple of 64. Each thread adds its vertex's morph target deltas by the weights the Animate pass wrote,
// then mixes its 4 joint matrices by their skin weights, and writes the posed vertex, with the base vertex's layout, to
// this slot's skinned vertices. Every view then reads those, so a vertex is posed once per frame, however many views
// draw it.

static const uint SKIN_GROUP_SIZE = 64;

[[vk::push_constant]]
ConstantBuffer<AnimatePushConstants> pc;

// The group's morph targets with a weight other than 0, read once for its 64 vertices.
groupshared float groupWeights[MAX_MORPH_TARGETS];
groupshared uint activeTargets[MAX_MORPH_TARGETS];
groupshared float activeWeights[MAX_MORPH_TARGETS];
groupshared uint activeCount;

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

// A half float's bits as a float, with bit math only: f16tof32 would ask the device for 16-bit types.
float getHalf(uint bits)
{
    uint sign = (bits & 0x8000) << 16;
    uint exponent = (bits >> 10) & 0x1F;
    uint mantissa = bits & 0x3FF;
    if (exponent == 0)
        return asfloat(sign | asuint(float(mantissa) * 5.9604645e-8));
    if (exponent == 31)
        return asfloat(sign | 0x7F800000 | (mantissa << 13));
    return asfloat(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

float2 unpackHalves(uint value)
{
    return float2(getHalf(value & 0xFFFF), getHalf(value >> 16));
}

float3 safeNormalize(float3 v, float3 fallback)
{
    float length2 = dot(v, v);
    return length2 > 1e-24 ? v * rsqrt(length2) : fallback;
}

[shader("compute")]
[numthreads(SKIN_GROUP_SIZE, 1, 1)]
void skinMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    // Every return before the barriers is uniform over the group.
    AnimationFrame *frame = pc.frame;
    if (groupId.x >= frame->entityCount)
        return;
    AnimatedEntity entity = frame->entities[groupId.x];
    if (entity.clip < 0)
        return;
    AnimationMeshData *mesh = entity.mesh;
    uint groupFirst = groupId.y * SKIN_GROUP_SIZE;
    if (groupFirst >= mesh->skinnedVertexCount)
        return;

    // The entity's skinned vertices are its posed sub-meshes' one after another.
    int found = -1;
    for (uint k = 0; k < mesh->subMeshCount; ++k)
    {
        AnimationSubMeshData candidate = mesh->subMeshes[k];
        if (candidate.vertexCount > 0 && groupFirst >= candidate.firstSkinnedVertex && groupFirst < candidate.firstSkinnedVertex + candidate.vertexCount)
        {
            found = int(k);
            break;
        }
    }
    if (found < 0)
        return;
    AnimationSubMeshData subMesh = mesh->subMeshes[found];
    GeometryData *geometry = frame->geometry + subMesh.geometryIndex;
    uint targetCount = geometry->morphOffset >= 0 ? geometry->morphTargetCount : 0;

    // Targets whose weight is 0 are left out, so they cost no read of their deltas.
    if (targetCount > 0)
    {
        float *weights = frame->weights + entity.firstWeight + subMesh.firstWeight;
        for (uint t = threadId.x; t < targetCount; t += SKIN_GROUP_SIZE)
            groupWeights[t] = weights[t];
        GroupMemoryBarrierWithGroupSync();
        if (threadId.x == 0)
        {
            uint count = 0;
            for (uint t = 0; t < targetCount; ++t)
            {
                if (groupWeights[t] != 0.0)
                {
                    activeTargets[count] = t;
                    activeWeights[count] = groupWeights[t];
                    ++count;
                }
            }
            activeCount = count;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint local = groupFirst + threadId.x - subMesh.firstSkinnedVertex;
    if (local >= subMesh.vertexCount)
        return;
    VertexFull source = frame->vertices[uint(geometry->vertexOffset) + local];

    // pos = base + sum of w_t * delta_t, the same for the normal and the tangent, which are renormalized.
    uint active = targetCount > 0 ? activeCount : 0;
    if (active > 0)
    {
        float3 normal = source.normal;
        float3 tangent = source.tangent.xyz;
        for (uint a = 0; a < active; ++a)
        {
            MorphDelta delta = frame->morphs[uint(geometry->morphOffset) + activeTargets[a] * subMesh.vertexCount + local];
            float weight = activeWeights[a];
            float2 normalXY = unpackHalves(delta.normalTangent[0]);
            float2 normalZTangentX = unpackHalves(delta.normalTangent[1]);
            float2 tangentYZ = unpackHalves(delta.normalTangent[2]);
            source.pos += weight * delta.position;
            normal += weight * float3(normalXY, normalZTangentX.x);
            tangent += weight * float3(normalZTangentX.y, tangentYZ);
        }
        source.normal = safeNormalize(normal, source.normal);
        // tangent.w = 0: the mesh has no tangents, and the shader builds one.
        if (source.tangent.w != 0.0)
            source.tangent.xyz = safeNormalize(tangent, source.tangent.xyz);
    }

    VertexFull posed = source;
    if (subMesh.jointCount > 0)
    {
        SkinVertex skin = frame->skins[uint(geometry->skinOffset) + local];
        uint4 joints = uint4(skin.joints.x & 0xFFFF, skin.joints.x >> 16, skin.joints.y & 0xFFFF, skin.joints.y >> 16);
        float4 weights = float4(skin.weights.x & 0xFFFF, skin.weights.x >> 16, skin.weights.y & 0xFFFF, skin.weights.y >> 16) / 65535.0;

        uint first = entity.firstJoint + subMesh.firstJoint;
        float3x4 m = weights.x * loadJoint(frame, first + joints.x) + weights.y * loadJoint(frame, first + joints.y) +
                     weights.z * loadJoint(frame, first + joints.z) + weights.w * loadJoint(frame, first + joints.w);
        float3x3 m3 = float3x3(m[0].xyz, m[1].xyz, m[2].xyz);

        posed.pos = mul(m, float4(source.pos, 1.0));
        posed.normal = mul(getNormalMatrix(m3), source.normal);
        // A mirroring mix (a joint scaled through 0) turns the bitangent around, as the vertex shader does for a
        // mirroring entity.
        posed.tangent = float4(mul(m3, source.tangent.xyz), determinant(m3) < 0.0 ? -source.tangent.w : source.tangent.w);
    }
    frame->skinnedVertices[entity.firstSkinnedVertex + groupFirst + threadId.x] = posed;
}
// clang-format on
