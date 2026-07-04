// clang-format off
#pragma once

#include <cstdint>

#include "vulkan/vulkan.h"

#include "rasm/core/types.h"
#include "rasm/core/entity.h"

namespace rasm
{
    class Engine;

    class Window
    {
        struct Extent
        {
            uint32_t width;
            uint32_t height;
        };

    public:
                        Window() = default;
                        Window(Engine *engine, uint32_t width, uint32_t height);

        WindowHandle    createWindow();
        VkSurfaceKHR    createSurfaceVk(WindowHandle handle, VkInstance instance) const;
        void            destroyWindow(WindowHandle handle);
        void            pollEvents(Entity &camera);

    protected:
        Engine *engine = nullptr;
        Extent extent{1080, 720};
    };
}
// clang-format on