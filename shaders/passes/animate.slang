// clang-format off
#include "animationTypes.slang"
#include "renderSpace.slang"

// The Animate pass: one group per row of the animated entity table. It samples the entity's clip at the frame's time
// into each node's local matrix and each morph weight, walks up the parents to each node's world matrix, writes each
// skin joint's matrix, and writes the matrices of the entity's animated sub-meshes, and a posed one's bounds. The
// threads go over the nodes, weights, joints and sub-meshes in strides, so a mesh may have any number of them.

static const uint ANIMATE_GROUP_SIZE = 64;

[[vk::push_constant]]
ConstantBuffer<AnimatePushConstants> pc;

float getClipTime(AnimatedEntity entity, float clock)
{
    float time = entity.phase + clock * entity.speed;
    if (entity.duration <= 0.0)
        return 0.0;
    if (entity.loop != 0)
        return time - floor(time / entity.duration) * entity.duration;
    return clamp(time, 0.0, entity.duration);
}

float4 loadValue(AnimationMeshData *mesh, AnimationSamplerData sampler, uint value)
{
    float *v = mesh->keys + sampler.firstValue + value * 4;
    return float4(v[0], v[1], v[2], v[3]);
}

// The shortest way between two unit quaternions.
float4 slerp(float4 a, float4 b, float t)
{
    float cosAngle = dot(a, b);
    if (cosAngle < 0.0)
    {
        b = -b;
        cosAngle = -cosAngle;
    }
    if (cosAngle > 0.9995)
        return normalize(lerp(a, b, t));
    float angle = acos(cosAngle);
    return (sin((1.0 - t) * angle) * a + sin(t * angle) * b) / sin(angle);
}

// glTF's sampling rules: before the first key the first value, after the last the last; STEP holds a key's value,
// LINEAR mixes two keys (slerp for a rotation), CUBICSPLINE is a Hermite curve with the keys' tangents.
float4 sampleChannel(AnimationMeshData *mesh, int samplerIndex, float time, bool rotation)
{
    AnimationSamplerData sampler = mesh->samplers[samplerIndex];
    float *times = mesh->keys + sampler.firstTime;
    bool cubic = sampler.interpolation == INTERPOLATION_CUBICSPLINE;
    uint last = sampler.keyCount - 1;
    if (last == 0 || time <= times[0])
        return loadValue(mesh, sampler, cubic ? 1 : 0);
    if (time >= times[last])
        return loadValue(mesh, sampler, cubic ? last * 3 + 1 : last);

    // times[low] <= time < times[high]
    uint low = 0;
    uint high = last;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (times[middle] <= time)
            low = middle;
        else
            high = middle;
    }
    float span = times[high] - times[low];
    float t = span > 0.0 ? (time - times[low]) / span : 0.0;

    if (sampler.interpolation == INTERPOLATION_STEP)
        return loadValue(mesh, sampler, low);
    if (!cubic)
    {
        float4 a = loadValue(mesh, sampler, low);
        float4 b = loadValue(mesh, sampler, high);
        return rotation ? slerp(a, b, t) : lerp(a, b, t);
    }

    float t2 = t * t;
    float t3 = t2 * t;
    float4 value0 = loadValue(mesh, sampler, low * 3 + 1);
    float4 out0 = loadValue(mesh, sampler, low * 3 + 2);
    float4 in1 = loadValue(mesh, sampler, high * 3);
    float4 value1 = loadValue(mesh, sampler, high * 3 + 1);
    float4 value = (2.0 * t3 - 3.0 * t2 + 1.0) * value0 + (t3 - 2.0 * t2 + t) * span * out0 + (-2.0 * t3 + 3.0 * t2) * value1 + (t3 - t2) * span * in1;
    return rotation ? normalize(value) : value;
}

// The rows of translate * rotate * scale.
float3x4 composeTrs(float3 t, float4 q, float3 s)
{
    float3x4 m;
    m[0] = float4((1.0 - 2.0 * (q.y * q.y + q.z * q.z)) * s.x, 2.0 * (q.x * q.y - q.z * q.w) * s.y, 2.0 * (q.x * q.z + q.y * q.w) * s.z, t.x);
    m[1] = float4(2.0 * (q.x * q.y + q.z * q.w) * s.x, (1.0 - 2.0 * (q.x * q.x + q.z * q.z)) * s.y, 2.0 * (q.y * q.z - q.x * q.w) * s.z, t.y);
    m[2] = float4(2.0 * (q.x * q.z - q.y * q.w) * s.x, 2.0 * (q.y * q.z + q.x * q.w) * s.y, (1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * s.z, t.z);
    return m;
}

// a * b for affine matrices given by their three rows.
float3x4 mulAffine(float3x4 a, float3x4 b)
{
    float3x4 m;
    for (uint i = 0; i < 3; ++i)
    {
        float4 row = a[i].x * b[0] + a[i].y * b[1] + a[i].z * b[2];
        m[i] = float4(row.xyz, row.w + a[i].w);
    }
    return m;
}

float3x4 loadRows(float4 *rows)
{
    float3x4 m;
    m[0] = rows[0];
    m[1] = rows[1];
    m[2] = rows[2];
    return m;
}

void storeRows(float4 *rows, float3x4 m)
{
    rows[0] = m[0];
    rows[1] = m[1];
    rows[2] = m[2];
}

// The matrix's largest stretch, bounded from above as getWorldSphere does (frustumCulling.slang).
float getStretch(float3x4 m)
{
    float3 c0 = float3(m[0].x, m[1].x, m[2].x);
    float3 c1 = float3(m[0].y, m[1].y, m[2].y);
    float3 c2 = float3(m[0].z, m[1].z, m[2].z);
    float d01 = abs(dot(c0, c1));
    float d02 = abs(dot(c0, c2));
    float d12 = abs(dot(c1, c2));
    return sqrt(max(max(dot(c0, c0) + d01 + d02, dot(c1, c1) + d01 + d12), dot(c2, c2) + d02 + d12));
}

// How far the sub-mesh's morph targets can move a vertex: target t moves it by at most |w_t| times its longest
// position change, and the moves add up.
float getMorphGrowth(AnimationFrame *frame, AnimationMeshData *mesh, AnimatedEntity entity, AnimationSubMeshData subMesh)
{
    GeometryData morphed = frame->geometry[subMesh.geometryIndex];
    if (morphed.morphOffset < 0)
        return 0.0;
    float growth = 0.0;
    for (uint t = 0; t < morphed.morphTargetCount; ++t)
        growth += abs(frame->weights[entity.firstWeight + subMesh.firstWeight + t]) * mesh->morphRadii[subMesh.firstMorphRadius + t];
    return growth;
}

// A joint's sphere, grown by the morph targets' moves, then moved by its joint matrix. A sphere with radius < 0 has no
// vertex.
float4 getPosedSphere(AnimationFrame *frame, AnimationMeshData *mesh, AnimatedEntity entity, AnimationSubMeshData subMesh, uint joint, float growth)
{
    float4 sphere = mesh->jointSpheres[subMesh.firstSphere + joint];
    if (sphere.w < 0.0)
        return sphere;
    float3x4 matrix = loadRows(frame->joints + (entity.firstJoint + subMesh.firstJoint + joint) * 3);
    return float4(mul(matrix, float4(sphere.xyz, 1.0)), (sphere.w + growth) * getStretch(matrix));
}

// A skinned vertex is a weighted mix of its joints' matrices applied to it, and each of them puts it inside that
// joint's posed sphere. A sphere around all the posed spheres is convex, so it holds every mix: the center of their
// box, the radius to the farthest edge.
float4 getSkinnedBounds(AnimationFrame *frame, AnimationMeshData *mesh, AnimatedEntity entity, AnimationSubMeshData subMesh, float growth)
{
    float3 low = float3(1e30);
    float3 high = float3(-1e30);
    for (uint j = 0; j < subMesh.jointCount; ++j)
    {
        float4 sphere = getPosedSphere(frame, mesh, entity, subMesh, j, growth);
        if (sphere.w < 0.0)
            continue;
        low = min(low, sphere.xyz - sphere.w);
        high = max(high, sphere.xyz + sphere.w);
    }
    if (low.x > high.x)
        return float4(0.0);
    float3 center = (low + high) * 0.5;
    float radius = 0.0;
    for (uint j = 0; j < subMesh.jointCount; ++j)
    {
        float4 sphere = getPosedSphere(frame, mesh, entity, subMesh, j, growth);
        if (sphere.w >= 0.0)
            radius = max(radius, length(sphere.xyz - center) + sphere.w);
    }
    return float4(center, radius);
}

// transpose(inverse(m)): the cofactors over the determinant.
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
[numthreads(ANIMATE_GROUP_SIZE, 1, 1)]
void animateMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    AnimationFrame *frame = pc.frame;
    // Uniform over the group, so no thread is left waiting at a barrier.
    if (groupId.x >= frame->entityCount)
        return;
    AnimatedEntity entity = frame->entities[groupId.x];
    if (entity.clip < 0)
        return;

    AnimationMeshData *mesh = entity.mesh;
    uint nodeCount = mesh->nodeCount;
    float time = getClipTime(entity, frame->clock);
    uint weightCount = mesh->weightCount;
    int32_t *channels = mesh->channels + uint(entity.clip) * (nodeCount * 3 + weightCount);
    float4 *locals = frame->pose + entity.firstNode * 6;
    float4 *worlds = locals + nodeCount * 3;

    for (uint n = threadId.x; n < nodeCount; n += ANIMATE_GROUP_SIZE)
    {
        AnimationNodeData node = mesh->nodes[n];
        int translation = channels[n * 3];
        int rotation = channels[n * 3 + 1];
        int scale = channels[n * 3 + 2];
        float3x4 local;
        if (translation < 0 && rotation < 0 && scale < 0)
        {
            local[0] = node.rest[0];
            local[1] = node.rest[1];
            local[2] = node.rest[2];
        }
        else
        {
            float3 t = translation >= 0 ? sampleChannel(mesh, translation, time, false).xyz : node.translation;
            float4 q = rotation >= 0 ? sampleChannel(mesh, rotation, time, true) : node.rotation;
            float3 s = scale >= 0 ? sampleChannel(mesh, scale, time, false).xyz : node.scale;
            local = composeTrs(t, normalize(q), s);
        }
        storeRows(locals + n * 3, local);
    }
    // A weight is setMorphWeights's, else its clip's, else its rest weight.
    for (uint w = threadId.x; w < weightCount; w += ANIMATE_GROUP_SIZE)
    {
        float weight = frame->weightOverrides[entity.firstWeight + w];
        if (asuint(weight) == NO_WEIGHT_OVERRIDE)
        {
            int sampler = channels[nodeCount * 3 + w];
            weight = sampler >= 0 ? sampleChannel(mesh, sampler, time, false).x : mesh->weights[w];
        }
        frame->weights[entity.firstWeight + w] = weight;
    }
    AllMemoryBarrierWithGroupSync();

    // Each node walks up its own parents, so no node waits for another's world matrix.
    for (uint n = threadId.x; n < nodeCount; n += ANIMATE_GROUP_SIZE)
    {
        float3x4 world = loadRows(locals + n * 3);
        int parent = mesh->nodes[n].parent;
        while (parent >= 0)
        {
            world = mulAffine(loadRows(locals + uint(parent) * 3), world);
            parent = mesh->nodes[parent].parent;
        }
        storeRows(worlds + n * 3, world);
    }
    AllMemoryBarrierWithGroupSync();

    // A joint's matrix moves a bind-pose vertex with the joint: the node's world matrix now, times the inverse of the
    // one it had in the bind pose.
    for (uint j = threadId.x; j < mesh->jointCount; j += ANIMATE_GROUP_SIZE)
    {
        AnimationJointData joint = mesh->joints[j];
        float3x4 inverseBind;
        inverseBind[0] = joint.inverseBind[0];
        inverseBind[1] = joint.inverseBind[1];
        inverseBind[2] = joint.inverseBind[2];
        storeRows(frame->joints + (entity.firstJoint + j) * 3, mulAffine(loadRows(worlds + joint.node * 3), inverseBind));
    }
    AllMemoryBarrierWithGroupSync();

    // The other slot holds the frame before only when that frame ran this pass, and this entity's pose then.
    bool reset = frame->previousSync + 1 != frame->sync || entity.resetSync > frame->previousSync;
    for (uint k = threadId.x; k < mesh->subMeshCount; k += ANIMATE_GROUP_SIZE)
    {
        AnimationSubMeshData subMesh = mesh->subMeshes[k];
        uint row = entity.firstTransform + k;

        // A skinned sub-mesh ignores its node (glTF): its joints place its vertices, and the entity places them all. A
        // morphed one that is not skinned follows its node, with the vertices the Skin pass morphs.
        // The entity's translation stays split in a high and a low part: the node's offset, turned and scaled by the
        // entity, is small and goes to the low part. Every view subtracts its own origin from them (toRenderSpace).
        InstanceTransform transform;
        float4x4 world = float4x4(float4(entity.entity[0].xyz, 0.0), float4(entity.entity[1].xyz, 0.0), float4(entity.entity[2].xyz, 0.0), float4(0.0, 0.0, 0.0, 1.0));
        transform.boundsCenter = float3(0.0);
        transform.boundsRadius = -1.0;
        transform.skinnedVertex = -1;
        transform.skinnedMotion = 0;
        float growth = getMorphGrowth(frame, mesh, entity, subMesh);
        if (subMesh.jointCount == 0)
        {
            float3x4 node = loadRows(worlds + subMesh.node * 3);
            world = mul(world, float4x4(node[0], node[1], node[2], float4(0.0, 0.0, 0.0, 1.0)));
            if (subMesh.vertexCount > 0)
            {
                GeometryData base = frame->geometry[subMesh.geometryIndex];
                transform.boundsCenter = base.boundsCenter;
                transform.boundsRadius = base.boundsRadius + growth;
            }
        }
        else
        {
            float4 bounds = getSkinnedBounds(frame, mesh, entity, subMesh, growth);
            transform.boundsCenter = bounds.xyz;
            transform.boundsRadius = bounds.w;
        }
        if (subMesh.vertexCount > 0)
        {
            transform.skinnedVertex = int(entity.firstSkinnedVertex + subMesh.firstSkinnedVertex);
            transform.skinnedMotion = reset ? 0 : 1;
        }
        for (uint i = 0; i < 3; ++i)
            transform.world[i] = float4(world[i].xyz, entity.entity[i].w);
        transform.translationLow = entity.translationLow + float3(world[0].w, world[1].w, world[2].w);
        transform.normalMatrix = getNormalMatrix((float3x3)world);
        // The frame before's rows, with its translation minus this one's (renderSpace.slang).
        InstanceTransform previous = reset ? transform : frame->previousTransforms[row];
        float3 moved = subtractSplit(float3(previous.world[0].w, previous.world[1].w, previous.world[2].w), previous.translationLow,
                                     float3(transform.world[0].w, transform.world[1].w, transform.world[2].w), transform.translationLow, pc.zero);
        for (uint i = 0; i < 3; ++i)
            transform.previousWorld[i] = float4(previous.world[i].xyz, moved[i]);
        frame->transforms[row] = transform;
    }
}
// clang-format on
