#pragma once

#include <cstdint>
#include <string>

namespace rasm
{
    template <typename Tag>
    struct Handle
    {
        uint32_t index = 0;
        uint32_t generation = 0;

        [[nodiscard]] bool isValid() const { return generation != 0; }
    };

    struct MeshTag;
    struct TextureTag;
    struct MaterialTag;
    struct SceneTag;
    struct EntityTag;

    using MeshHandle = Handle<MeshTag>;
    using TextureHandle = Handle<TextureTag>;
    using MaterialHandle = Handle<MaterialTag>;
    using SceneId = Handle<SceneTag>;
    using EntityHandle = Handle<EntityTag>;

    struct EngineConfig
    {
        std::string appName;
        int windowWidth;
        int windowHeight;
        bool enableValidation;
    };

    std::string rasm_hello();

}
