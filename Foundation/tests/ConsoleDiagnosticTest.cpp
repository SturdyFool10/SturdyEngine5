#include <Foundation/ConsoleDiagnostic.hpp>

#include <iostream>
#include <string>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const SFT::Foundation::ConsoleDiagnostic diagnostic{
        .severity = SFT::Foundation::DiagnosticSeverity::Error,
        .code = "engine.surface.register",
        .summary = "could not create a render surface for the primary window",
        .context = "Engine::Application::spawn_sdl3_managed_window",
        .cause_code = "core.graphics.unsupported",
        .cause = "the selected GPU does not support presentation\r\non this display\n",
        .details = "requested backend: Vulkan\r\nrequested queue: present\n",
        .help = "verify that the display driver exposes Vulkan presentation support",
    };
    const std::string expected =
        "error[engine.surface.register]: could not create a render surface for the primary window\n"
        "  --> Engine::Application::spawn_sdl3_managed_window\n"
        "   |\n"
        "   = cause[core.graphics.unsupported]: the selected GPU does not support presentation\n"
        "     on this display\n"
        "   | requested backend: Vulkan\n"
        "   | requested queue: present\n"
        "   = help: verify that the display driver exposes Vulkan presentation support";

    const std::string rendered = SFT::Foundation::format_console_diagnostic(diagnostic);
    if (rendered != expected) {
        std::cerr << "unexpected diagnostic:\n" << rendered << '\n';
        return 1;
    }
    return 0;
}
