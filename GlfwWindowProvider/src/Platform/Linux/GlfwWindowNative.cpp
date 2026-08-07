#include <Foundation/src/Foundation.hpp>

#include <cstdint>
#include <expected>

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(Success)
#undef Success
#endif
#if defined(None)
#undef None
#endif
#if defined(Always)
#undef Always
#endif
#if defined(Bool)
#undef Bool
#endif
#endif

#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/GLFW/GlfwWindowNative.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::unexpected;
using std::uintptr_t;

namespace SFT::Platform::Windowing::GLFW::Detail {

    expected<NativeWindowHandle, WindowError> native_window_handle(void *window_handle) noexcept {
        ZoneScopedN("Windowing::GLFW::Detail::native_window_handle");
#if defined(__linux__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Linux native handle requires a live window."});
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            NativeWindowHandle handle{NativeWindowSystem::Wayland, glfwGetWaylandDisplay(), glfwGetWaylandWindow(window)};
            if (!handle.display || !handle.window) [[unlikely]] {
                ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux Wayland native handle missing display or surface: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Wayland native handle is incomplete."});
            }
            return handle;
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            NativeWindowHandle handle{
                NativeWindowSystem::X11,
                glfwGetX11Display(),
                reinterpret_cast<void *>(static_cast<uintptr_t>(glfwGetX11Window(window))),
            };
            if (!handle.display || !handle.window) [[unlikely]] {
                ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux X11 native handle missing display or window: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW X11 native handle is incomplete."});
            }
            return handle;
        }

        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Linux native handle platform is unsupported."});
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Linux native handles are only available on Linux builds."});
#endif
    }

} // namespace SFT::Platform::Windowing::GLFW::Detail
