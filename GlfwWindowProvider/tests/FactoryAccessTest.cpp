#include <Platform/Window/GLFW/GLFW.hpp>

int main() {
    const SFT::Platform::Windowing::WindowFactory factory =
        &SFT::Platform::Windowing::GLFW::create_window;
    return factory != nullptr ? 0 : 1;
}
