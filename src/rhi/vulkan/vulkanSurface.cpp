#include "rhi/vulkan/vulkanSurface.h"

// The only file with a windowing system's types. volk is built without VK_USE_PLATFORM_* (src/CMakeLists.txt), so the
// surface functions come from vkGetInstanceProcAddr, and each platform header is included after the types it names.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "vulkan/vulkan_win32.h"
#elif defined(__APPLE__)
#include "vulkan/vulkan_metal.h"
#else
// The X11, XCB and Wayland types, declared as their own headers do, so RASM builds without their development packages.
#include <cstdint>
struct _XDisplay;
struct xcb_connection_t;
using Display = _XDisplay;
using Window = unsigned long;
using VisualID = unsigned long;
using xcb_window_t = uint32_t;
using xcb_visualid_t = uint32_t;
#include "vulkan/vulkan_wayland.h"
#include "vulkan/vulkan_xcb.h"
#include "vulkan/vulkan_xlib.h"
#endif

#include "spdlog/spdlog.h"

#include <cstdint>

namespace rasm::rhi
{
    namespace
    {
        PFN_vkVoidFunction getFunction(VkInstance instance, const char *name)
        {
            const PFN_vkVoidFunction function = vkGetInstanceProcAddr(instance, name);
            if (function == nullptr)
                spdlog::error("The Vulkan instance has no {}: its surface extension is missing.", name);
            return function;
        }

        bool check(VkResult result, const char *what)
        {
            if (result == VK_SUCCESS)
                return true;
            spdlog::error("{} failed: VkResult {}.", what, static_cast<int>(result));
            return false;
        }
    }

    bool createVulkanSurface(const NativeWindow &window, VkInstance instance, VkSurfaceKHR *surface)
    {
        if (window.window == nullptr)
        {
            spdlog::error("The native window has no handle.");
            return false;
        }
        switch (window.type)
        {
#if defined(_WIN32)
        case NativeWindowType::WINDOWS:
        {
            const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(getFunction(instance, "vkCreateWin32SurfaceKHR"));
            const VkWin32SurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                      .hinstance = window.display != nullptr ? static_cast<HINSTANCE>(window.display) : GetModuleHandleW(nullptr),
                                                      .hwnd = static_cast<HWND>(window.window)};
            return create != nullptr && check(create(instance, &info, nullptr, surface), "vkCreateWin32SurfaceKHR");
        }
#elif defined(__APPLE__)
        case NativeWindowType::METAL:
        {
            const auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(getFunction(instance, "vkCreateMetalSurfaceEXT"));
            const VkMetalSurfaceCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                                      .pLayer = static_cast<const CAMetalLayer *>(window.window)};
            return create != nullptr && check(create(instance, &info, nullptr, surface), "vkCreateMetalSurfaceEXT");
        }
#else
        case NativeWindowType::X11:
        {
            const auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(getFunction(instance, "vkCreateXlibSurfaceKHR"));
            const VkXlibSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                                                     .dpy = static_cast<Display *>(window.display),
                                                     .window = static_cast<::Window>(reinterpret_cast<uintptr_t>(window.window))};
            return create != nullptr && check(create(instance, &info, nullptr, surface), "vkCreateXlibSurfaceKHR");
        }
        case NativeWindowType::XCB:
        {
            const auto create = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(getFunction(instance, "vkCreateXcbSurfaceKHR"));
            const VkXcbSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                                    .connection = static_cast<xcb_connection_t *>(window.display),
                                                    .window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(window.window))};
            return create != nullptr && check(create(instance, &info, nullptr, surface), "vkCreateXcbSurfaceKHR");
        }
        case NativeWindowType::WAYLAND:
        {
            const auto create = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(getFunction(instance, "vkCreateWaylandSurfaceKHR"));
            const VkWaylandSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                                        .display = static_cast<wl_display *>(window.display),
                                                        .surface = static_cast<wl_surface *>(window.window)};
            return create != nullptr && check(create(instance, &info, nullptr, surface), "vkCreateWaylandSurfaceKHR");
        }
#endif
        default:
            spdlog::error("NativeWindowType {} has no Vulkan surface on this operating system.", static_cast<int>(window.type));
            return false;
        }
    }
}
