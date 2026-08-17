#include <Platform/Window/GLFW/GLFW.hpp>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const SFT::Platform::Windowing::WindowFactory factory =
        &SFT::Platform::Windowing::GLFW::create_window;
    return factory != nullptr ? 0 : 1;
}
