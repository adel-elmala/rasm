// clang-format off
// Dear ImGui's draw lists (src/renderer/ui.cpp): one indexed draw per ImDrawCmd, clipped by the scissor. The vertex
// shader pulls each ImDrawVert from the frame slot's vertex buffer, so the pipeline has no vertex input.
// ImGui's colors are sRGB numbers, and it means them to be blended as numbers. With RASM_UI_ENCODED the target is the
// swapchain's UNORM view, which stores and blends them as they are. Without it the target is sRGB: the shader decodes
// the colors to linear, the hardware encodes the blend, and blending happens in linear light.
// Either way a texture shows its stored numbers: an sRGB one is sampled as linear and encoded again for the UNORM view,
// a UNORM one is decoded for the sRGB target.

// Must match ImDrawVert (20 bytes).
struct UiVertex
{
    float2  position;       // ImGui's display coordinates: points, from the top-left corner
    float2  uv;
    uint    color;          // RGBA8, sRGB-encoded, red in the low byte
};

// Must match gfx::UiPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct UiPushConstants
{
    UiVertex*   vertices;   // the draw's first vertex (its list's base plus ImDrawCmd::VtxOffset)
    uint        texture;    // bindless index of the draw's texture, | SRGB_TEXTURE_BIT for a texture in an sRGB format
    uint        displaySize; // width | height << 16, in ImGui's points
};

static const uint SRGB_TEXTURE_BIT = 0x80000000;  // Must match SRGB_TEXTURE_BIT in src/renderer/ui.cpp.

[[vk::push_constant]]
ConstantBuffer<UiPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR;        // alpha straight; linear without RASM_UI_ENCODED
};

float3 decodeSrgb(float3 encoded)
{
    return select(encoded <= 0.04045, encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4));
}

float3 encodeSrgb(float3 linear)
{
    return select(linear <= 0.0031308, linear * 12.92, 1.055 * pow(linear, 1.0 / 2.4) - 0.055);
}

// The index buffer holds each list's own 16-bit indices; the list's place in the vertex buffer comes in through the
// push constants, since D3D12's SV_VertexID leaves out a draw's base vertex.
[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID)
{
    UiVertex vertex = pc.vertices[vertexID];
    float2 displaySize = float2(pc.displaySize & 0xFFFF, pc.displaySize >> 16);
    float4 color = float4(vertex.color & 0xFF, (vertex.color >> 8) & 0xFF, (vertex.color >> 16) & 0xFF, vertex.color >> 24) / 255.0;

    VSOutput output;
    // Clip space is Y-up, ImGui's display Y-down.
    output.position = float4(vertex.position.x / displaySize.x * 2.0 - 1.0, 1.0 - vertex.position.y / displaySize.y * 2.0, 0.0, 1.0);
    output.uv       = vertex.uv;
#if defined(RASM_UI_ENCODED)
    output.color    = color;
#else
    output.color    = float4(decodeSrgb(color.rgb), color.a);
#endif
    return output;
}

// The pipeline blends premultiplied alpha.
[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float4 texel = gTextures[pc.texture & ~SRGB_TEXTURE_BIT].Sample(input.uv);
    bool srgbTexture = (pc.texture & SRGB_TEXTURE_BIT) != 0;
#if defined(RASM_UI_ENCODED)
    if (srgbTexture)
        texel.rgb = encodeSrgb(texel.rgb);
#else
    if (!srgbTexture)
        texel.rgb = decodeSrgb(texel.rgb);
#endif
    float4 color = input.color * texel;
    return float4(color.rgb * color.a, color.a);
}
