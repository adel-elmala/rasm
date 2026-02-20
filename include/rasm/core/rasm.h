#pragma once

#include <cstdint>
#include <string>

namespace rasm
{
    template <typename Tag>
    struct Handle
    {
        uint64_t index = 0;
        uint64_t generation = 0;

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
    using SceneHandle = Handle<SceneTag>;
    using EntityHandle = Handle<EntityTag>;

    struct EngineConfig
    {
        std::string appName{};
        int windowWidth = 0;
        int windowHeight = 0;
        bool enableValidation = false;
    };

    std::string rasm_hello();

}
