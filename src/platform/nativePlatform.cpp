#include "platform/platform.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        // The application's window: RASM makes a surface on it and reads nothing from it. The application runs the
        // event loop and passes on the input, the size and the scale (Renderer::addInputEvent, setWindowSize,
        // setWindowContentScale).
        class NativePlatform final : public Platform
        {
        public:
            PlatformType type() const override
            {
                return PlatformType::NATIVE;
            }

            Window createWindow(const WindowDesc &desc) override
            {
                const NativeWindow &native = desc.native;
                const uint32_t width = desc.width;
                const uint32_t height = desc.height;
                if (native.type == NativeWindowType::NONE || native.window == nullptr)
                {
                    spdlog::error("PlatformType::NATIVE needs a window with a type and a handle (RendererConfig::nativeWindow, WindowDesc::native).");
                    return Window{};
                }
                Window window{};
                window.native = native;
                window.width = width;
                window.height = height;
                window.pixelWidth = native.pixelWidth != 0 ? native.pixelWidth : width;
                window.pixelHeight = native.pixelHeight != 0 ? native.pixelHeight : height;
                window.contentScale = native.contentScale > 0.0f ? native.contentScale : 1.0f;
                return window;
            }

            void destroyWindow(const Window &) override {}

            void pollEvents(Window *, uint32_t, std::vector<InputEvent> &, bool &) override {}
        };
    }

    std::unique_ptr<Platform> createNativePlatform()
    {
        return std::make_unique<NativePlatform>();
    }
}
