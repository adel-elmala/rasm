#include "platform/platform.h"

namespace rasm
{
    namespace
    {
        // No window and no events: the application drives every frame, and the frames go to the offscreen swapchain.
        // Its input, if any, comes through Renderer::addInputEvent.
        class HeadlessPlatform final : public Platform
        {
        public:
            PlatformType type() const override
            {
                return PlatformType::HEADLESS;
            }

            Window createWindow(const WindowDesc &desc) override
            {
                Window window{};
                window.width = desc.width;
                window.height = desc.height;
                window.pixelWidth = desc.width;
                window.pixelHeight = desc.height;
                return window;
            }

            void destroyWindow(const Window &) override {}

            void pollEvents(Window *, uint32_t, std::vector<InputEvent> &, bool &) override {}
        };
    }

    std::unique_ptr<Platform> createHeadlessPlatform()
    {
        return std::make_unique<HeadlessPlatform>();
    }
}
