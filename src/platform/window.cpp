#include "rasm/window.h"
#include "rasm/engine.h"
#include "rasm/transform.h"
#include "rasm/camera.h"

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

        int pixelWidth = 0;
        int pixelHeight = 0;
        if (!SDL_GetWindowSizeInPixels(wind, &pixelWidth, &pixelHeight))
        {
            spdlog::warn("Failed to query window size in pixels: {}", SDL_GetError());
            pixelWidth = static_cast<int>(width);
            pixelHeight = static_cast<int>(height);
        }

        Window window{};
        window.handle = {.index = reinterpret_cast<uint64_t>(wind), .generation = 1}; // Placeholder handle generation logic
        window.width = width;
        window.height = height;
        window.pixelWidth = static_cast<uint32_t>(pixelWidth);
        window.pixelHeight = static_cast<uint32_t>(pixelHeight);

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
        profiler.updateDeltaTime();

        glm::vec2 dragStart = mainWindow.lastMousePos;
        glm::vec2 dragEnd = mainWindow.lastMousePos;
        float wheelDelta = 0.0f;

        // Nothing is rendered while minimized, so block until an event arrives instead of spinning.
        if (mainWindow.minimized)
            SDL_WaitEventTimeout(nullptr, 100);

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
                if (event.key.key == SDLK_ESCAPE)
                {
                    spdlog::info("Escape key pressed, closing window.");
                    isRunning = false;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
                mainWindow.lastMousePos = toNDC(glm::vec2(event.button.x, event.button.y), mainWindow);
                dragStart = mainWindow.lastMousePos;
                dragEnd = mainWindow.lastMousePos;
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

                dragEnd = toNDC(glm::vec2(event.motion.x, event.motion.y), mainWindow);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                wheelDelta += event.wheel.y;
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            {
                // Logical size, only used for mouse coordinates.
                mainWindow.width = event.window.data1;
                mainWindow.height = event.window.data2;
                break;
            }
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            {
                // The swapchain, depth textures, viewport and camera aspect follow the pixel size (B17).
                mainWindow.resized = true;
                mainWindow.pixelWidth = event.window.data1;
                mainWindow.pixelHeight = event.window.data2;
                break;
            }
            case SDL_EVENT_WINDOW_MINIMIZED:
            {
                mainWindow.minimized = true;
                break;
            }
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            {
                mainWindow.minimized = false;
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            {
                SDL_ResetKeyboard();
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

        const bool *keys = SDL_GetKeyboardState(nullptr);

        CameraInput input{};
        input.deltaTime = profiler.deltaTime;
        input.wheelDelta = wheelDelta;
        if (mainWindow.mouseDragActive)
        {
            input.mouseStart = dragStart;
            input.mouseEnd = dragEnd;
        }
        input.forward = keys[SDL_SCANCODE_W];
        input.backward = keys[SDL_SCANCODE_S];
        input.left = keys[SDL_SCANCODE_A];
        input.right = keys[SDL_SCANCODE_D];

        updateCamera(camera, input);
        mainWindow.lastMousePos = dragEnd;
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