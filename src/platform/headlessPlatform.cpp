#include "platform/platform.h"

namespace rasm
{
    namespace
    {
        // No window and no events: the application drives every frame, and the frames go to the offscreen swapchain.
        // Its input, if any, comes through Engine::addInputEvent.
        class HeadlessPlatform final : public Platform
        {
        public:
            Window createWindow(uint32_t width, uint32_t height) override
            {
                Window window{};
                window.width = width;
                window.height = height;
                window.pixelWidth = width;
                window.pixelHeight = height;
                return window;
            }

            void destroyWindow(const Window &) override {}

            void pollEvents(Window &, std::vector<InputEvent> &, bool &) override {}
        };
    }

    std::unique_ptr<Platform> createHeadlessPlatform()
    {
        return std::make_unique<HeadlessPlatform>();
    }
}
