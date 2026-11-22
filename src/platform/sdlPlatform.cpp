#include "platform/platform.h"
#include "platform/uiPlatform.h"
#include "platform/windowSurface.h"

#include <algorithm>

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        // Convert screen coordinates to NDC. Both axes are normalized by the smaller
        // window dimension so that the virtual trackball stays circular on non-square windows.
        glm::vec2 toNDC(const glm::vec2 &screenPos, const Window &window)
        {
            float minDim = static_cast<float>(std::min(window.width, window.height));
            float x = (2.0f * screenPos.x - static_cast<float>(window.width)) / minDim;
            // Screen y grows downward; NDC y grows upward like the camera's +Y (D8).
            float y = (static_cast<float>(window.height) - 2.0f * screenPos.y) / minDim;
            return glm::vec2(x, y);
        }

        // SDL loads the Vulkan loader for its windows, and the device takes it from SDL (getWindowVulkanLoader). volk
        // does the same fallback: dlopen may not search /usr/local/lib, where the Vulkan SDK installs the loader on macOS.
        bool loadVulkanLoader()
        {
            if (SDL_Vulkan_LoadLibrary(nullptr))
                return true;
#if defined(__APPLE__)
            if (SDL_Vulkan_LoadLibrary("/usr/local/lib/libvulkan.dylib"))
                return true;
#endif
            spdlog::error("No Vulkan loader found ({}). Install a GPU driver with Vulkan 1.4.", SDL_GetError());
            return false;
        }

        class SdlPlatform final : public Platform
        {
        public:
            Window createWindow(uint32_t width, uint32_t height) override
            {
                if (!SDL_Init(SDL_INIT_VIDEO))
                {
                    spdlog::error("Failed to initialize SDL: {}", SDL_GetError());
                    return Window{};
                }

                if (!loadVulkanLoader())
                {
                    SDL_Quit();
                    return Window{};
                }

                auto wind = SDL_CreateWindow("RASM Engine", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                if (!wind)
                {
                    spdlog::error("Failed to create SDL window: {}", SDL_GetError());
                    SDL_Vulkan_UnloadLibrary();
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

            void destroyWindow(const Window &window) override
            {
                if (window.handle.isValid())
                {
                    SDL_DestroyWindow(reinterpret_cast<SDL_Window *>(window.handle.index));
                    SDL_Vulkan_UnloadLibrary();
                    SDL_Quit();
                }
            }

            void pollEvents(Window &window, bool uiActive, PlatformEvents &events) override
            {
                glm::vec2 dragStart = window.lastMousePos;
                glm::vec2 dragEnd = window.lastMousePos;
                float wheelDelta = 0.0f;

                // Nothing is rendered while minimized, so block until an event arrives instead of spinning.
                if (window.minimized)
                    SDL_WaitEventTimeout(nullptr, 100);

                // The UI takes the mouse over its windows and the keys while it has the focus, as the last ImGui frame decided.
                // Function keys and Escape still work, except while a text field takes typing. A UI that builds no frame
                // (no RASM window, no callback) gets no events.
                UiInputCapture capture = {};
#if defined(RASM_IMGUI)
                if (uiActive)
                    capture = getUiInputCapture();
#else
                (void)uiActive;
#endif

                SDL_Event event;
                while (SDL_PollEvent(&event))
                {
#if defined(RASM_IMGUI)
                    if (uiActive)
                        processUiEvent(&event);
#endif
                    switch (event.type)
                    {
                    case SDL_EVENT_QUIT:
                    {
                        spdlog::info("Quit event received, shutting down.");
                        events.quit = true;
                        break;
                    }
                    case SDL_EVENT_KEY_DOWN:
                    {
                        if (capture.text)
                            break;
                        if (event.key.key == SDLK_ESCAPE)
                        {
                            spdlog::info("Escape key pressed, closing window.");
                            events.quit = true;
                        }
                        if (event.key.key == SDLK_F1 && !event.key.repeat)
                            ++events.statsToggles;
                        if (event.key.key == SDLK_F2 && !event.key.repeat)
                            ++events.forcedLodSteps;
                        if (event.key.key == SDLK_F3 && !event.key.repeat)
                            events.dumpGraph = true;
                        break;
                    }
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    {
                        if (capture.mouse)
                            break;
                        window.lastMousePos = toNDC(glm::vec2(event.button.x, event.button.y), window);
                        dragStart = window.lastMousePos;
                        dragEnd = window.lastMousePos;
                        window.mouseDragActive = true;
                        break;
                    }
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                    {
                        window.mouseDragActive = false;
                        break;
                    }
                    case SDL_EVENT_MOUSE_MOTION:
                    {
                        if (!window.mouseDragActive)
                            break;
                        dragEnd = toNDC(glm::vec2(event.motion.x, event.motion.y), window);
                        break;
                    }
                    case SDL_EVENT_MOUSE_WHEEL:
                    {
                        if (capture.mouse)
                            break;
                        wheelDelta += event.wheel.y;
                        break;
                    }
                    case SDL_EVENT_WINDOW_RESIZED:
                    {
                        // Logical size, only used for mouse coordinates.
                        window.width = event.window.data1;
                        window.height = event.window.data2;
                        break;
                    }
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    {
                        // The swapchain, depth textures, viewport and camera aspect follow the pixel size (B17).
                        window.resized = true;
                        window.pixelWidth = event.window.data1;
                        window.pixelHeight = event.window.data2;
                        break;
                    }
                    case SDL_EVENT_WINDOW_MINIMIZED:
                    {
                        window.minimized = true;
                        break;
                    }
                    case SDL_EVENT_WINDOW_RESTORED:
                    case SDL_EVENT_WINDOW_MAXIMIZED:
                    {
                        window.minimized = false;
                        break;
                    }
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                    {
                        SDL_ResetKeyboard();
                        break;
                    }
                    default:
                    {
                        spdlog::debug("Received event of type: {}", event.type);
                        break;
                    }
                    }
                }

                const bool *keys = SDL_GetKeyboardState(nullptr);

                events.camera.wheelDelta = wheelDelta;
                if (window.mouseDragActive)
                {
                    events.camera.mouseStart = dragStart;
                    events.camera.mouseEnd = dragEnd;
                }
                if (!capture.keyboard)
                {
                    events.camera.forward = keys[SDL_SCANCODE_W];
                    events.camera.backward = keys[SDL_SCANCODE_S];
                    events.camera.left = keys[SDL_SCANCODE_A];
                    events.camera.right = keys[SDL_SCANCODE_D];
                }
                window.lastMousePos = dragEnd;
            }
        };
    }

    std::unique_ptr<Platform> createSdlPlatform()
    {
        return std::make_unique<SdlPlatform>();
    }

    PFN_vkGetInstanceProcAddr getWindowVulkanLoader()
    {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }

    bool createWindowSurface(const Window &window, VkInstance instance, VkSurfaceKHR *surface)
    {
        if (!SDL_Vulkan_CreateSurface(reinterpret_cast<SDL_Window *>(window.handle.index), instance, nullptr, surface))
        {
            spdlog::error("Failed to create Vulkan surface: {}", SDL_GetError());
            return false;
        }
        return true;
    }
}
