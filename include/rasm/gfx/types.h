#pragma once
namespace rasm
{
    enum class Backend
    {
        Vulkan,
        DX12,
        Metal
    };

    enum class BufferUsage
    {
        Vertex,
        Index,
        Uniform,
        Storage
    };

    enum class TextureFormat
    {
        RGBA8,
        RGBA16F,
        Depth24Stencil8
    };

    enum class ShaderType
    {
        Vertex,
        Fragment,
        Compute
    };

}