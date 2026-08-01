#include <Foundation/src/Foundation.hpp>

#include <expected>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/GLFW/GlfwWindowNative.hpp>

using std::expected;
using std::unexpected;

namespace SFT::Platform::Windowing::GLFW::Detail {

    expected<NativeWindowHandle, WindowError> native_window_handle(void *window_handle) noexcept {
#if defined(__APPLE__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::Platform::Windowing::Detail::window_error("GLFW Cocoa native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle requires a live window."});
        }

        NativeWindowHandle handle{NativeWindowSystem::Cocoa, nullptr, glfwGetCocoaWindow(window)};
        if (!handle.window) [[unlikely]] {
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle is incomplete."});
        }
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Cocoa native handles are only available on Apple builds."});
#endif
    }

} // namespace SFT::Platform::Windowing::GLFW::Detail
