// clang-format off
#pragma once

// The animation structs. Each must match its rasm::gfx struct in src/renderer/uberMaterialTypes.h; core.slang includes
// this file, and shaders/passes/animate.slang and skin.slang include it alone, since they have push constants of their
// own.

#include "geometryTypes.slang"

// Must match rasm::AnimationInterpolation.
static const uint   INTERPOLATION_STEP          = 0;
static const uint   INTERPOLATION_LINEAR        = 1;
static const uint   INTERPOLATION_CUBICSPLINE   = 2;
// Must match NO_WEIGHT_OVERRIDE in src/renderer/animation.cpp: a quiet NaN, compared by its bits, where setMorphWeights
// sets no weight.
static const uint   NO_WEIGHT_OVERRIDE          = 0x7FC00000;

// What the Animate pass writes for an animated sub-mesh, in place of its instance row's matrices. A skinned one's
// matrices are its entity's, and its vertices and bounds are posed.
struct InstanceTransform
{
    float4              world[3];           // the rows of the affine world matrix; w: the translation's high part
    float3              translationLow;     // the translation is w + translationLow
    float3x3            normalMatrix;
    float4              previousWorld[3];   // the rows the frame before drew; w: its translation minus this one
    float3              boundsCenter;       // a skinned sub-mesh's sphere around its posed vertices, object space
    float               boundsRadius;       // < 0: the geometry's bounds
    int32_t             skinnedVertex;      // its first vertex in SceneData::skinnedVertsPtr, -1: the geometry pool's
    uint32_t            skinnedMotion;      // 1: SceneData::previousSkinnedVertsPtr holds the frame before's
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

// An animated sub-mesh: a rigid one follows a node, a skinned one (jointCount > 0) is moved by joints. A skinned or
// morphed one is posed by the Skin pass.
struct AnimationSubMeshData
{
    uint32_t            node;               // the node a rigid one follows
    uint32_t            jointCount;         // 0: rigid
    uint32_t            firstJoint;         // in AnimationMeshData::joints
    uint32_t            firstSphere;        // in AnimationMeshData::jointSpheres, jointCount of them
    uint32_t            geometryIndex;
    uint32_t            vertexCount;        // 0: not posed by the Skin pass
    uint32_t            firstSkinnedVertex; // among the entity's skinned vertices, a multiple of 64
    uint32_t            firstWeight;        // among the entity's morph weights
    uint32_t            firstMorphRadius;   // in AnimationMeshData::morphRadii, one per morph target
};

// A skin's joint: the node that moves it, and the inverse of the node's world matrix in the bind pose.
struct AnimationJointData
{
    float4              inverseBind[3];     // its rows
    uint32_t            node;
};

// One mesh's animations, in the animation pool.
struct AnimationMeshData
{
    AnimationNodeData*      nodes;
    AnimationSamplerData*   samplers;
    int32_t*                channels;       // per clip: per node the sampler of translation, rotation, scale, then per weight its sampler; -1: rest
    AnimationSubMeshData*   subMeshes;      // per animated sub-mesh
    float*                  keys;
    AnimationJointData*     joints;
    float4*                 jointSpheres;   // center and radius around the vertices a joint moves, object space; radius < 0: none
    float*                  weights;        // the morph weights at rest, weightCount of them
    float*                  morphRadii;     // per morph target of each sub-mesh: its largest position change
    uint32_t                nodeCount;
    uint32_t                subMeshCount;
    uint32_t                jointCount;
    uint32_t                skinnedVertexCount; // per entity
    uint32_t                weightCount;    // per entity
    uint32_t                padding;
};

// A row of the animated entity table, written by the CPU when an entity plays, stops or moves.
struct AnimatedEntity
{
    float4              entity[3];          // the rows of the entity's matrix; w: the translation's high part
    float3              translationLow;
    uint32_t            rowPadding;
    AnimationMeshData*  mesh;
    float               phase;              // the clip's time at the clock's base
    float               speed;
    float               duration;
    int32_t             clip;               // -1: no clip plays
    uint32_t            loop;
    uint32_t            firstTransform;     // its first row in AnimationFrame::transforms
    uint32_t            firstNode;          // its first node in AnimationFrame::pose
    uint32_t            resetSync;          // the sync whose frame has no motion from the frame before
    uint32_t            firstJoint;         // its first joint matrix in AnimationFrame::joints
    uint32_t            firstSkinnedVertex; // its first vertex in AnimationFrame::skinnedVertices
    uint32_t            firstWeight;        // its first weight in AnimationFrame::weights and weightOverrides
    uint32_t            padding;
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
    float4*             joints;             // this slot's: 3 rows per joint matrix, global * inverse bind
    VertexFull*         vertices;           // the geometry pool
    SkinVertex*         skins;              // the geometry pool's skin stream
    GeometryData*       geometry;           // the geometry table
    VertexFull*         skinnedVertices;    // this slot's, written by the Skin pass
    float*              weights;            // this slot's morph weights, written by the Animate pass
    float*              weightOverrides;    // this slot's setMorphWeights values, NO_WEIGHT_OVERRIDE where the clip's count
    MorphDelta*         morphs;             // the geometry pool's morph stream
};

struct AnimatePushConstants
{
    AnimationFrame*     frame;
    uint32_t            zero;               // 0, which the shader compiler cannot know (renderSpace.slang)
    uint32_t            padding;
};
// clang-format on
