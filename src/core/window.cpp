#include "rasm/core/window.h"
#include "rasm/core/engine.h"
#include "rasm/core/transform.h"
#include "rasm/core/camera.h"

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include "spdlog/spdlog.h"

namespace rasm
{

    WindowHandle Engine::createWindow(uint32_t width, uint32_t height)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            spdlog::error("Failed to initialize SDL: {}", SDL_GetError());
            return WindowHandle{};
        }

        auto wind = SDL_CreateWindow("RASM Engine", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!wind)
        {
            spdlog::error("Failed to create SDL window: {}", SDL_GetError());
            SDL_Quit();
            return WindowHandle{};
        }

        return {.index = reinterpret_cast<uint64_t>(wind), .generation = 1}; // Placeholder handle generation logic
    }

    VkSurfaceKHR Engine::createSurfaceVk(WindowHandle handle, VkInstance instance) const
    {
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(reinterpret_cast<SDL_Window *>(handle.index), instance, nullptr, &surface))
        {
            spdlog::error("Failed to create Vulkan surface: {}", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    }

    void Engine::pollEvents(CameraHandle camera)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            {
                spdlog::info("Quit event received, shutting down.");
                isRunning = false;
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            {
                auto &cam = registery.cameras[camera.index].transform;

                switch (event.key.key)
                {
                case SDLK_ESCAPE:
                {
                    spdlog::info("Escape key pressed, closing window.");
                    isRunning = false;
                    break;
                }
                case SDLK_W:
                {
                    cam.position += glm::vec3(0.0f, 0.0f, 1.0f); // Move forward
                    break;
                }
                case SDLK_S:
                {
                    cam.position += glm::vec3(0.0f, 0.0f, -1.0f); // Move backward
                    break;
                }
                case SDLK_A:
                {
                    cam.position += glm::vec3(1.0f, 0.0f, 0.0f); // Move left
                    break;
                }
                case SDLK_D:
                {
                    cam.position += glm::vec3(-1.0f, 0.0f, 0.0f); // Move right
                    break;
                }
                }
            break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            {
                config.windowWidth = event.window.data1;
                config.windowHeight = event.window.data2;
                resized = true;
                break;
            }
            default:
            {
                // Handle other events (e.g., input, window resize, etc.)
                spdlog::debug("Received event of type: {}", event.type);
                break;
            }
            }
        }
    }

    void Engine::destroyWindow(WindowHandle handle)
    {
        if (handle.isValid())
        {
            SDL_DestroyWindow(reinterpret_cast<SDL_Window *>(handle.index));
            SDL_Quit();
        }
    }
}