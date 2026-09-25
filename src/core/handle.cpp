#include "rasm/core/handle.h"

namespace rasm
{
    MeshHandle HandleManager::getNextMeshHandle()
    {
        return MeshHandle{nextHandle.mesh++, 1};
    }

    BufferHandle HandleManager::getNextBufferHandle()
    {
        return BufferHandle{nextHandle.buffer++, 1};
    }

    TextureHandle HandleManager::getNextTextureHandle()
    {
        return TextureHandle{nextHandle.texture++, 1};
    }

    ShaderHandle HandleManager::getNextShaderHandle()
    {
        return ShaderHandle{nextHandle.shader++, 1};
    }

    PipelineHandle HandleManager::getNextPipelineHandle()
    {
        return PipelineHandle{nextHandle.pipeline++, 1};
    }

    CommandPoolHandle HandleManager::getNextCommandPoolHandle()
    {
        return CommandPoolHandle{nextHandle.commandPool++, 1};
    }

    CommandBufferHandle HandleManager::getNextCommandBufferHandle()
    {
        return CommandBufferHandle{nextHandle.commandBuffer++, 1};
    }

    SemaphoreHandle HandleManager::getNextSemaphoreHandle()
    {
        return SemaphoreHandle{nextHandle.semaphore++, 1};
    }

    FenceHandle HandleManager::getNextFenceHandle()
    {
        return FenceHandle{nextHandle.fence++, 1};
    }

    SwapchainHandle HandleManager::getNextSwapchainHandle()
    {
        return SwapchainHandle{nextHandle.swapchain++, 1};
    }

    RenderTargetHandle HandleManager::getNextRenderTargetHandle()
    {
        return RenderTargetHandle{nextHandle.renderTarget++, 1};
    }

    DescriptorSetLayoutHandle HandleManager::getNextDescriptorSetLayoutHandle()
    {
        return DescriptorSetLayoutHandle{nextHandle.descriptorSetLayout++, 1};
    }

    DescriptorSetHandle HandleManager::getNextDescriptorSetHandle()
    {
        return DescriptorSetHandle{nextHandle.descriptorSet++, 1};
    }

    DescriptorPoolHandle HandleManager::getNextDescriptorPoolHandle()
    {
        return DescriptorPoolHandle{nextHandle.descriptorPool++, 1};
    }

    MaterialHandle HandleManager::getNextMaterialHandle()
    {
        return MaterialHandle{nextHandle.material++, 1};
    }

    CameraHandle HandleManager::getNextCameraHandle()
    {
        return CameraHandle{nextHandle.camera++, 1};
    }

    EntityHandle HandleManager::getNextEntityHandle()
    {
        return EntityHandle{nextHandle.entity++, 1};
    }

    LightHandle HandleManager::getNextLightHandle()
    {
        return LightHandle{nextHandle.light++, 1};
    }

    SceneHandle HandleManager::getNextSceneHandle()
    {
        return SceneHandle{nextHandle.scene++, 1};
    }
} // namespace rasm
