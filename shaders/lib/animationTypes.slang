// clang-format off
#pragma once

// The animation structs. Each must match its rasm::gfx struct in src/renderer/uberMaterialTypes.h; core.slang includes
// this file, and shaders/passes/animate.slang includes it alone, since it has push constants of its own.

// Must match rasm::AnimationInterpolation.
static const uint   INTERPOLATION_STEP          = 0;
static const uint   INTERPOLATION_LINEAR        = 1;
static const uint   INTERPOLATION_CUBICSPLINE   = 2;

// What the Animate pass writes for an animated sub-mesh, in place of its instance row's matrices.
struct InstanceTransform
{
    float4x4            worldMatrix;
    float3x3            normalMatrix;
    float4              previousWorld[3];   // the rows of the affine world matrix the frame before drew
};

// A node of a mesh's tree that a clip can move (rasm::AnimationNode).
struct AnimationNodeData
{
    float4              rest[3];            // the rows of the local matrix at rest
    float3              translation;
    int32_t             parent;             // an earlier node, -1 for a root
    float4              rotation;           // quaternion x, y, z, w
    float3              scale;
    uint32_t            padding;
};

struct AnimationSamplerData
{
    uint32_t            firstTime;          // in AnimationMeshData::keys
    uint32_t            firstValue;         // in keys, 4 floats per value
    uint32_t            keyCount;
    uint32_t            interpolation;      // INTERPOLATION_*
};

// One mesh's animations, in the animation pool.
struct AnimationMeshData
{
    AnimationNodeData*      nodes;
    AnimationSamplerData*   samplers;
    int32_t*                channels;       // per clip, node and path: the sampler of translation, rotation, scale; -1: rest
    uint32_t*               subMeshNodes;   // per animated sub-mesh: the node it follows
    float*                  keys;
    uint32_t                nodeCount;
    uint32_t                subMeshCount;
};

// A row of the animated entity table, written by the CPU when an entity plays, stops or moves.
struct AnimatedEntity
{
    float4x4            entityMatrix;
    AnimationMeshData*  mesh;
    float               phase;              // the clip's time at the clock's base
    float               speed;
    float               duration;
    int32_t             clip;               // -1: no clip plays
    uint32_t            loop;
    uint32_t            firstTransform;     // its first row in AnimationFrame::transforms
    uint32_t            firstNode;          // its first node in AnimationFrame::pose
    uint32_t            resetSync;          // the sync whose frame has no motion from the frame before
};

struct AnimationFrame
{
    AnimatedEntity*     entities;           // this slot's table
    InstanceTransform*  transforms;         // this slot's
    InstanceTransform*  previousTransforms; // the other slot's, written by the frame before
    float4*             pose;               // 3 rows per local matrix, then 3 per world matrix of each node
    float               clock;              // seconds since the clock's base
    uint32_t            sync;
    uint32_t            entityCount;
    uint32_t            previousSync;       // the sync of the last frame that ran the pass
};

struct AnimatePushConstants
{
    AnimationFrame*     frame;
    uint32_t            padding[2];
};
// clang-format on
