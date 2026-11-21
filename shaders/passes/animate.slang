// clang-format off
#include "animationTypes.slang"

// The Animate pass: one group per row of the animated entity table. It samples the entity's clip at the frame's time
// into each node's local matrix, walks up the parents to each node's world matrix, and writes the matrices of the
// entity's animated sub-meshes. The threads go over the nodes and sub-meshes in strides, so a mesh may have any number
// of them.

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
    int32_t *channels = mesh->channels + uint(entity.clip) * nodeCount * 3;
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

    bool reset = entity.resetSync == frame->sync;
    for (uint k = threadId.x; k < mesh->subMeshCount; k += ANIMATE_GROUP_SIZE)
    {
        float3x4 node = loadRows(worlds + mesh->subMeshNodes[k] * 3);
        float4x4 world = mul(entity.entityMatrix, float4x4(node[0], node[1], node[2], float4(0.0, 0.0, 0.0, 1.0)));
        uint row = entity.firstTransform + k;

        InstanceTransform transform;
        transform.worldMatrix = world;
        transform.normalMatrix = getNormalMatrix((float3x3)world);
        float4x4 previous = reset ? world : frame->previousTransforms[row].worldMatrix;
        transform.previousWorld[0] = previous[0];
        transform.previousWorld[1] = previous[1];
        transform.previousWorld[2] = previous[2];
        frame->transforms[row] = transform;
    }
}
// clang-format on
