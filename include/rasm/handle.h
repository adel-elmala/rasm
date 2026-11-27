// clang-format off
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rasm
{
    inline std::size_t hashCombine(std::size_t seed, std::size_t value)
    {
        return seed ^ (value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6) + (seed >> 2));
    }

    template <typename Tag>
    struct Handle
    {
        uint64_t index      = 0;
        uint64_t generation = 0;

        [[nodiscard]] bool isValid() const { return generation != 0; }
        bool operator==(const Handle& other) const { return index == other.index && generation == other.generation; }
    };

    struct HandleHash
    {
        template <typename Tag>
        std::size_t operator()(const Handle<Tag>& handle) const
        {
            return hashCombine(std::hash<uint64_t>()(handle.index), std::hash<uint64_t>()(handle.generation));
        }
    };

    struct MeshTag;
    struct BufferTag;
    struct TextureTag;
    struct MaterialTag;
    struct PipelineTag;
    struct CommandBufferTag;
    struct FenceTag;
    struct SwapchainTag;
    struct ShaderTag;
    struct SceneTag;
    struct EntityTag;
    struct CameraTag;
    struct WindowTag;
    struct ResourceTag;
    struct RenderTargetTag;
    struct LightTag;
    struct DescriptorSetLayoutTag;
    struct DescriptorPoolTag;
    struct DescriptorSetTag;
    struct QueryPoolTag;
    struct EnvironmentTag;
    struct ReadbackTag;
    struct TileTag;
    struct FontTag;
    struct PickTag;
    struct ExportTag;

    using MeshHandle                = Handle<MeshTag>;
    using BufferHandle              = Handle<BufferTag>;
    using TextureHandle             = Handle<TextureTag>;
    using MaterialHandle            = Handle<MaterialTag>;
    using PipelineHandle            = Handle<PipelineTag>;
    using CommandBufferHandle       = Handle<CommandBufferTag>;
    using FenceHandle               = Handle<FenceTag>;
    using SwapchainHandle           = Handle<SwapchainTag>;
    using ShaderHandle              = Handle<ShaderTag>;
    using SceneHandle               = Handle<SceneTag>;
    using EntityHandle              = Handle<EntityTag>;
    using CameraHandle              = Handle<CameraTag>;
    using LightHandle               = Handle<LightTag>;
    using WindowHandle              = Handle<WindowTag>;
    using ResourceHandle            = Handle<ResourceTag>;
    using RenderTargetHandle        = Handle<RenderTargetTag>;
    using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
    using DescriptorPoolHandle      = Handle<DescriptorPoolTag>;
    using DescriptorSetHandle       = Handle<DescriptorSetTag>;
    using QueryPoolHandle           = Handle<QueryPoolTag>;
    using EnvironmentHandle         = Handle<EnvironmentTag>;
    using ReadbackHandle            = Handle<ReadbackTag>;
    using TileHandle                = Handle<TileTag>;
    using FontHandle                = Handle<FontTag>;
    using PickHandle                = Handle<PickTag>;
    using ExportHandle              = Handle<ExportTag>;


    struct HandleSlot
    {
        uint64_t    index       = 0;
        uint64_t    generation  = 0;
    };

    // A handle's generation never passes it, so the C API packs a handle into one uint64 (rasm_c.h) with no loss.
    constexpr uint64_t              MAX_HANDLE_GENERATION   = UINT32_MAX;

    // Hands out handle indices from 1 to limit - 1, each with the generation its slot has now. A released index goes
    // on a free list (last released, first reused) with its generation one higher, so every handle made for it before is
    // stale. A slot whose generation reaches maxGeneration is never reused, so a generation can't wrap back to an old one.
    class HandlePool
    {
    public:
                                    HandlePool() = default;
        explicit                    HandlePool(uint64_t limit, uint64_t maxGeneration = MAX_HANDLE_GENERATION); // indices 1 .. limit - 1

        HandleSlot                  allocate();                                         // {0, 0} when every index is in use
        bool                        release(uint64_t index, uint64_t generation);       // false for a stale or invalid handle
        bool                        isAlive(uint64_t index, uint64_t generation) const;
        uint64_t                    size() const;                                       // one past the highest index handed out: rows a table indexed by handle needs
        uint64_t                    liveCount() const;

    private:
        uint64_t                    indexLimit      = UINT32_MAX;
        uint64_t                    generationLimit = MAX_HANDLE_GENERATION;
        std::vector<uint64_t>       generations     = {0};                              // by index; index 0 is never handed out
        std::vector<uint8_t>        alive           = {0};
        std::vector<uint64_t>       freeIndices;
        uint64_t                    live            = 0;
    };

} // namespace rasm

// clang-format on