#include "rasm/core/window.h"
#include "rasm/core/engine.h"

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    Window::Window(Engine *engine, uint32_t width, uint32_t height) : engine{engine}, extent{width, height} {}

    WindowHandle Window::createWindow()
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            spdlog::error("Failed to initialize SDL: {}", SDL_GetError());
            return WindowHandle{};
        }

        auto wind = SDL_CreateWindow("RASM Engine", extent.width, extent.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!wind)
        {
            spdlog::error("Failed to create SDL window: {}", SDL_GetError());
            return WindowHandle{};
        }

        return {.index = reinterpret_cast<uint64_t>(wind), .generation = 1}; // Placeholder handle generation logic
    }

    VkSurfaceKHR Window::createSurfaceVk(WindowHandle handle, VkInstance instance) const
    {
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(reinterpret_cast<SDL_Window*>(handle.index), instance, nullptr, &surface)) {
            spdlog::error("Failed to create Vulkan surface: {}", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    }


    void Window::pollEvents()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                // Handle quit event, e.g., set a flag in the engine to stop the main loop
                spdlog::info("Quit event received, shutting down.");
                engine->isRunning = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                spdlog::info("Window close event received, shutting down.");
                engine->isRunning = false;
            }
             else
            {
                // Handle other events (e.g., input, window resize, etc.)
                spdlog::debug("Received event of type: {}", event.type);
            }
        }
    }

    void Window::destroyWindow(WindowHandle handle)
    {
        if (handle.isValid())
        {
            SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(handle.index));
            SDL_Quit();
        }
    }
}