// clang-format off
#pragma once

#include "core.slang"
#include "points.slang"

// Glyph chunks (rasm/glyphs.h, passes/glyphCull.slang). A chunk lives in the point pool (SceneData::pointsPtr, 8 B
// units) at its GeometryData::vertexOffset: a header of GLYPH_CHUNK_HEADER_UNITS, then two units per glyph (its
// position, a float3, and its value: RGBA8 or a float scalar), then with GLYPH_CHUNK_ORIENTED a unit per glyph (a
// quaternion, four snorm16), then with GLYPH_CHUNK_SCALED a unit per glyph (three half floats and a spare one). Its
// instance row's meshletVisibility names its shape's geometry row (writeInstanceRows), whose LODs the glyphs draw.

// GlyphChunkHeader::flags and size, and the cull's constants. Must match rasm::gfx::GLYPH_* in uberMaterialTypes.h.
static const uint   GLYPH_CHUNK_COLORS          = 1 << 0;
static const uint   GLYPH_CHUNK_SCALARS         = 1 << 1;
static const uint   GLYPH_CHUNK_ORIENTED        = 1 << 2;
static const uint   GLYPH_CHUNK_SCALED          = 1 << 3;
static const uint   GLYPH_CHUNK_HEADER_UNITS    = 4;
static const uint   GLYPH_BATCH_GLYPHS          = 256;
static const uint   GLYPH_MAX_CHUNKS            = 1 << 16;
// glyphSlotsPtr: a culled glyph, else its place among its chunk's visible glyphs of its LOD, the LOD above.
static const uint   GLYPH_CULLED                = 0xFFFFFFFF;
static const uint   GLYPH_LOD_SHIFT             = 24;
static const uint   GLYPH_PLACE_MASK            = (1 << GLYPH_LOD_SHIFT) - 1;

struct GlyphChunk
{
    uint            base;               // its first unit in the point pool
    uint            glyphCount;
    uint            flags;
    float           maxScale;
};

struct Glyph
{
    float3          position;           // in its entity's space
    uint            value;              // RGBA8, or a scalar's bits
    float4          orientation;        // quaternion x, y, z, w
    float3          scale;
};

GlyphChunk loadGlyphChunk(SceneData *scene, uint base)
{
    uint2 u0            = scene->pointsPtr[base];
    uint2 u1            = scene->pointsPtr[base + 1];
    GlyphChunk chunk;
    chunk.base          = base;
    chunk.glyphCount    = u0.x;
    chunk.flags         = u0.y;
    chunk.maxScale      = asfloat(u1.x);
    return chunk;
}

float unpackSnorm16(uint bits)
{
    return max(float(int(bits << 16) >> 16) / 32767.0, -1.0);
}

// A half float's low 16 bits, decoded by hand: f16tof32 makes Slang declare the Float16 and Int16 capabilities, which
// RASM does not enable. Scales are finite and above 0 (buildGlyphChunk), so infinities and NaN need no case.
float decodeHalf(uint bits)
{
    uint exponent       = (bits >> 10) & 0x1F;
    uint mantissa       = bits & 0x3FF;
    float magnitude     = exponent == 0 ? float(mantissa) * (1.0 / 16777216.0) : asfloat((exponent + 112) << 23 | mantissa << 13);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

Glyph loadGlyph(SceneData *scene, GlyphChunk chunk, uint glyph)
{
    uint first          = chunk.base + GLYPH_CHUNK_HEADER_UNITS;
    uint2 a             = scene->pointsPtr[first + 2 * glyph];
    uint2 b             = scene->pointsPtr[first + 2 * glyph + 1];
    Glyph result;
    result.position     = asfloat(uint3(a.x, a.y, b.x));
    result.value        = b.y;
    result.orientation  = float4(0.0, 0.0, 0.0, 1.0);
    result.scale        = float3(1.0);
    uint next           = first + 2 * chunk.glyphCount;
    if ((chunk.flags & GLYPH_CHUNK_ORIENTED) != 0)
    {
        uint2 q             = scene->pointsPtr[next + glyph];
        float4 unpacked     = float4(unpackSnorm16(q.x), unpackSnorm16(q.x >> 16), unpackSnorm16(q.y), unpackSnorm16(q.y >> 16));
        result.orientation  = unpacked / max(length(unpacked), 1e-6);
        next               += chunk.glyphCount;
    }
    if ((chunk.flags & GLYPH_CHUNK_SCALED) != 0)
    {
        uint2 h             = scene->pointsPtr[next + glyph];
        result.scale        = float3(decodeHalf(h.x), decodeHalf(h.x >> 16), decodeHalf(h.y));
    }
    return result;
}

float3x3 getQuaternionMatrix(float4 q)
{
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    return float3x3(1.0 - (yy + zz), xy - wz, xz + wy,
                    xy + wz, 1.0 - (xx + zz), yz - wx,
                    xz - wy, yz + wx, 1.0 - (xx + yy));
}

// The glyph's place in its entity's space: translate, then rotate, then scale its shape, as Transform::toMatrix.
float4x4 getGlyphMatrix(Glyph glyph)
{
    float3x3 rotation   = getQuaternionMatrix(glyph.orientation);
    float3x3 m          = float3x3(rotation[0] * glyph.scale, rotation[1] * glyph.scale, rotation[2] * glyph.scale);
    return float4x4(float4(m[0], glyph.position.x), float4(m[1], glyph.position.y), float4(m[2], glyph.position.z), float4(0.0, 0.0, 0.0, 1.0));
}

// The inverse transpose of its linear part: rotation times 1 / scale.
float3x3 getGlyphNormalMatrix(Glyph glyph)
{
    float3x3 rotation   = getQuaternionMatrix(glyph.orientation);
    float3 inverse      = 1.0 / max(abs(glyph.scale), float3(1e-12)) * sign(glyph.scale);
    return float3x3(rotation[0] * inverse, rotation[1] * inverse, rotation[2] * inverse);
}

// How far a glyph's shape reaches from the glyph's position, in the shape's units.
float getShapeReach(GeometryData *shape)
{
    return length(shape->boundsCenter) + shape->boundsRadius;
}

// A glyph chunk's sphere holds its positions: its draw's sphere grows by the shape's reach at its largest scale.
float getGlyphChunkPadding(SceneData *scene, InstanceData *instance, GeometryData *geometry)
{
    int shape           = instance->meshletVisibility;
    if (shape < 0)
        return 0.0;
    GlyphChunk chunk    = loadGlyphChunk(scene, uint(geometry->vertexOffset));
    return chunk.maxScale * getShapeReach(scene->geometryPtr + uint(shape));
}

// The glyph's color before its material's base color: its own, white without one, or its scalar through the colormap,
// which sets alpha to -1 for a scalar outside the range without clamp: the glyph is not drawn.
float4 getGlyphColor(Sampler2D textures[], PointData style, GlyphChunk chunk, Glyph glyph)
{
    if ((style.flags & POINT_SCALAR) != 0 && (chunk.flags & GLYPH_CHUNK_SCALARS) != 0 && style.colormap != INVALID_TEXTURE_INDEX)
    {
        float t         = getColormapPosition(style, asfloat(glyph.value));
        if (t == POINT_OUT_OF_RANGE)
            return float4(0.0, 0.0, 0.0, -1.0);
        // Uniform over a draw: each of the glyph draw's commands draws one chunk.
        return textures[style.colormap].SampleLevel(getColormapCoordinate(t), 0.0);
    }
    if ((chunk.flags & GLYPH_CHUNK_COLORS) == 0)
        return float4(1.0);
    uint packed         = glyph.value;
    float4 color        = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    return float4(decodeSrgb(color.r), decodeSrgb(color.g), decodeSrgb(color.b), color.a);
}

// The cull pass keeps a glyph chunk in view (cull.slang's countMain): a slot of the view's glyph buffer, its batches of
// GLYPH_BATCH_GLYPHS, each a group of the glyph cull's dispatches, and a draw command per LOD of its shape. The batches
// are taken with a compare-exchange that adds only what fits, so every batch below the final count is written; a chunk
// that does not fit is not drawn this frame (the buffer holds every chunk of the point run, so none should).
bool appendGlyphChunk(SceneData *scene, uint row, uint glyphCount, uint lodCount, bool weighted)
{
    GlyphCullHeader *header = scene->glyphCullPtr;
    uint slot           = 0;
    InterlockedAdd(header->chunkCount, 1u, slot);
    if (slot >= min(scene->glyphChunkCapacity, GLYPH_MAX_CHUNKS))
        return false;
    uint batches        = (glyphCount + GLYPH_BATCH_GLYPHS - 1) / GLYPH_BATCH_GLYPHS;
    uint first          = 0;
    InterlockedAdd(header->groups, 0u, first);
    for (;;)
    {
        if (first + batches > scene->glyphBatchCapacity)
            return false;
        uint seen       = 0;
        InterlockedCompareExchange(header->groups, first, first + batches, seen);
        if (seen == first)
            break;
        first           = seen;
    }
    // A weighted chunk's commands go after the others' room: the weighted draw draws them (glyphCull.cpp).
    uint firstCommand   = 0;
    if (weighted)
    {
        InterlockedAdd(header->weightedCommandCount, lodCount, firstCommand);
        firstCommand   += scene->glyphChunkCapacity * MAX_MESH_LODS;
    }
    else
    {
        InterlockedAdd(header->commandCount, lodCount, firstCommand);
    }
    GlyphChunkSlot *chunk = scene->glyphChunksPtr + slot;
    chunk->row          = row;
    chunk->firstBatch   = first;
    chunk->firstCommand = firstCommand;
    chunk->lodCount     = lodCount;
    for (uint l = 0; l < MAX_MESH_LODS; ++l)
        chunk->counts[l] = 0;
    for (uint b = 0; b < batches; ++b)
        scene->glyphBatchesPtr[first + b] = slot << 8 | b;
    return true;
}
// clang-format on
