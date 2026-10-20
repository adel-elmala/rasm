#include "rasm/core/window.h"
#include "rasm/core/engine.h"
#include "rasm/core/transform.h"
#include "rasm/core/camera.h"

#include <algorithm>

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include "spdlog/spdlog.h"

namespace rasm
{

    Window Engine::createWindow(uint32_t width, uint32_t height)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            spdlog::error("Failed to initialize SDL: {}", SDL_GetError());
            return Window{};
        }

        auto wind = SDL_CreateWindow("RASM Engine", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!wind)
        {
            spdlog::error("Failed to create SDL window: {}", SDL_GetError());
            SDL_Quit();
            return Window{};
        }

        Window window{};
        window.handle = {.index = reinterpret_cast<uint64_t>(wind), .generation = 1}; // Placeholder handle generation logic
        window.width = width;
        window.height = height;

        return window;
    }

    VkSurfaceKHR Engine::createSurfaceVk(const Window &window, VkInstance instance) const
    {
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(reinterpret_cast<SDL_Window *>(window.handle.index), instance, nullptr, &surface))
        {
            spdlog::error("Failed to create Vulkan surface: {}", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    }

    // Convert screen coordinates to NDC. Both axes are normalized by the smaller
    // window dimension so that the virtual trackball stays circular on non-square windows.
    glm::vec2 toNDC(const glm::vec2 &screenPos, const Window &window)
    {
        float minDim = static_cast<float>(std::min(window.width, window.height));
        float x = (2.0f * screenPos.x - static_cast<float>(window.width)) / minDim;
        float y = (2.0f * screenPos.y - static_cast<float>(window.height)) / minDim;
        return glm::vec2(x, y);
    }

    void Engine::pollEvents(CameraHandle camera)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            CameraInput input{.key = CameraInput::Key::NONE};

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
                    input.key = CameraInput::Key::W;
                    break;
                }
                case SDLK_S:
                {
                    input.key = CameraInput::Key::S;
                    break;
                }
                case SDLK_A:
                {
                    input.key = CameraInput::Key::A;
                    break;
                }
                case SDLK_D:
                {
                    input.key = CameraInput::Key::D;
                    break;
                }
                }

                // Free-fly movement is driven by key events.
                if (input.key != CameraInput::Key::NONE)
                {
                    updateCamera(camera, input);
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
                mainWindow.lastMousePos = toNDC(glm::vec2(event.button.x, event.button.y), mainWindow);
                mainWindow.mouseDragActive = true;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                mainWindow.mouseDragActive = false;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            {
                if (!mainWindow.mouseDragActive)
                {
                    break;
                }

                input.mouseStart = mainWindow.lastMousePos;
                input.mouseEnd = toNDC(glm::vec2(event.motion.x, event.motion.y), mainWindow);

                updateCamera(camera, input);
                mainWindow.lastMousePos = input.mouseEnd;
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                input.wheelDelta = event.wheel.y;
                updateCamera(camera, input);
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            {
                mainWindow.resized = true;
                mainWindow.width = event.window.data1;
                mainWindow.height = event.window.data2;
                updateCameraAspectRatio(static_cast<float>(mainWindow.width) / static_cast<float>(mainWindow.height));
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

    void Engine::destroyWindow(const Window &window)
    {
        if (window.handle.isValid())
        {
            SDL_DestroyWindow(reinterpret_cast<SDL_Window *>(window.handle.index));
            SDL_Quit();
        }
    }
}