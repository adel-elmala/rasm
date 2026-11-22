// clang-format off
#pragma once

#include "rasm/handle.h"
#include "rasm/scene.h"
#include "rasm/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rasm::gfx
{
    struct SceneData;
}

namespace rasm
{
    // What the bake makes. The environment's own cube is a quarter of the image's width (its faces cover 90 degrees
    // of the image's 360), a power of two between the two limits.
    constexpr uint32_t          ENVIRONMENT_MIN_CUBE_SIZE   = 64;
    constexpr uint32_t          ENVIRONMENT_MAX_CUBE_SIZE   = 1024;
    constexpr uint32_t          SPECULAR_CUBE_SIZE          = 256;
    constexpr uint32_t          SPECULAR_MIPS               = 6;        // roughness 0, 0.2, ... 1
    constexpr uint32_t          IRRADIANCE_CUBE_SIZE        = 32;
    constexpr uint32_t          BRDF_LUT_SIZE               = 128;

    constexpr uint32_t          getMipCount(uint32_t size)
    {
        uint32_t count = 1;
        for (; size > 1; size /= 2)
            ++count;
        return count;
    }

    constexpr uint32_t          getEnvironmentCubeSize(uint32_t imageWidth)
    {
        uint32_t size = ENVIRONMENT_MIN_CUBE_SIZE;
        while (size * 2 <= imageWidth / 4 && size < ENVIRONMENT_MAX_CUBE_SIZE)
            size *= 2;
        return size;
    }

    // Storage slots of one frame's bake, below RESERVED_STORAGE_IMAGES: the LUT, then the environment cube's mips, the specular
    // cube's, the irradiance.
    constexpr uint32_t          BRDF_LUT_STORAGE_SLOT       = 0;
    constexpr uint32_t          ENVIRONMENT_STORAGE_SLOT    = 1;
    static_assert(ENVIRONMENT_STORAGE_SLOT + getMipCount(ENVIRONMENT_MAX_CUBE_SIZE) + SPECULAR_MIPS + 1 <= RESERVED_STORAGE_IMAGES,
                  "a bake needs a storage slot for each mip it writes");

    enum class BakeState
    {
        LOADING,                // decoding on a worker
        WAITING,                // its textures exist; the next frame that can records the bake
        RECORDED,               // this frame's commands bake it, before any pass, so this frame may use it
        READY,
        FAILED,
    };

    struct Environment
    {
        std::string             path;
        BakeState               state                       = BakeState::LOADING;
        TextureHandle           source;                     // the decoded image, destroyed once the bake has run
        TextureHandle           cube;                       // the sky, and what the filters read, with every mip
        TextureHandle           specular;
        TextureHandle           irradiance;
        uint32_t                sourceSlot                  = INVALID_TEXTURE_INDEX;
        uint32_t                cubeSlot                    = INVALID_TEXTURE_INDEX;
        uint32_t                specularSlot                = INVALID_TEXTURE_INDEX;
        uint32_t                irradianceSlot              = INVALID_TEXTURE_INDEX;
        uint32_t                cubeSize                    = 0;
        uint32_t                specularSize                = 0;
        uint64_t                bakeValue                   = 0;    // the frame fence value of the frame that baked it
        uint64_t                uploadValue                 = 0;    // the load uploads' value its image is on the GPU at: it waits for it
    };

    struct Environments
    {
        std::vector<Environment>    environments;           // by handle index
        TextureHandle               brdfLut;
        uint32_t                    brdfLutSlot             = INVALID_TEXTURE_INDEX;
        BakeState                   brdfLutState            = BakeState::WAITING;
        uint64_t                    storageFreeValue        = 0;    // the storage slots may be rewritten once the frame fence passes it
        bool                        bakeDestroyed           = false; // an environment this frame bakes was destroyed: finishEnvironmentBakes sets storageFreeValue
        PipelineHandle              cubePipeline;
        PipelineHandle              downsamplePipeline;
        PipelineHandle              specularPipeline;
        PipelineHandle              irradiancePipeline;
        PipelineHandle              brdfPipeline;
    };

    bool                            isBakeUsable(BakeState state);
    bool                            writeSceneEnvironment(const Environments& environments, const SceneEnvironment& environment, gfx::SceneData& sceneData);
}
// clang-format on
