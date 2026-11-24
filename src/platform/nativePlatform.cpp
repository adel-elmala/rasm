#include "platform/platform.h"

#include "spdlog/spdlog.h"

namespace rasm
{
    namespace
    {
        // The application's window: RASM makes a surface on it and reads nothing from it. The application runs the
        // event loop and passes on the input, the size and the scale (Engine::addInputEvent, setWindowSize,
        // setWindowContentScale).
        class NativePlatform final : public Platform
        {
        public:
            explicit NativePlatform(const NativeWindow &native) : native(native) {}

            Window createWindow(uint32_t width, uint32_t height) override
            {
                if (native.type == NativeWindowType::NONE || native.window == nullptr)
                {
                    spdlog::error("PlatformType::NATIVE needs EngineConfig::nativeWindow with a type and a window handle.");
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

            void pollEvents(Window &, std::vector<InputEvent> &, bool &) override {}

        private:
            NativeWindow native;
        };
    }

    std::unique_ptr<Platform> createNativePlatform(const NativeWindow &native)
    {
        return std::make_unique<NativePlatform>(native);
    }
}
