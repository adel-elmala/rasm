// clang-format off
#pragma once

// The faces of a cube in Vulkan's order (+X, -X, +Y, -Y, +Z, -Z): the axis a face looks along, and the directions in
// which its texel coordinates s and t grow (Vulkan's cube map face table). Must match localShadows.cpp.
static const float3 CUBE_FORWARD[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 CUBE_S[6]       = { float3(0, 0, -1), float3(0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0) };
static const float3 CUBE_T[6]       = { float3(0, -1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1), float3(0, -1, 0), float3(0, -1, 0) };

// The face a direction from the cube's centre falls on: the one of its biggest axis.
uint getCubeFace(float3 d)
{
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
        return d.x >= 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return d.y >= 0.0 ? 2 : 3;
    return d.z >= 0.0 ? 4 : 5;
}

// The direction from the cube's centre through point st (0 to 1 across the face) of a face.
float3 getCubeDirection(uint face, float2 st)
{
    return normalize(CUBE_FORWARD[face] + CUBE_S[face] * (st.x * 2.0 - 1.0) + CUBE_T[face] * (st.y * 2.0 - 1.0));
}

// clang-format on
