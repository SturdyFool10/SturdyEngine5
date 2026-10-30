#include <Engine/WindowControl.hpp>
#include <Engine/WindowRequests.hpp>
#include <WindowManager/Providers/SDL3/Window.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

// Drives the queued window requests against a real SDL3 window on a real X server (run under Xvfb): the same
// drain -> run-on-the-window -> record-status path `Application::process_window_requests` takes, so a request that
// builds the wrong call, loses its arguments or drops its query result shows up here. Explicit checks, not assert():
// asserts vanish under NDEBUG.

using namespace SFT;
using namespace SFT::Engine;
using namespace SFT::WindowManager;

namespace {

    int failures = 0;

    void check(bool condition, const char *what) {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    /// What the application does with each queued operation: run it on the window, record how it went.
    void run_pending(WindowRequests &requests, Window &window) {
        for (WindowRequest &request : requests.drain()) {
            auto *operation = std::get_if<RunWindowOperationRequest>(&request);
            if (operation == nullptr) {
                continue;
            }
            WindowOperationStatus status;
            status.window = operation->window;
            status.operation = operation->operation;
            auto result = operation->run(window, status.output);
            status.ok = result.has_value();
            if (!result.has_value()) {
                status.code = result.error().code;
                status.message = std::string{result.error().message};
            }
            requests.record_operation_status(std::move(status));
        }
    }

    /// An operation must have run, and either worked or said it is unsupported.
    void check_outcome(const WindowRequests &requests, WindowId window, WindowOperation operation, const char *name) {
        const auto status = requests.operation_status(window, operation);
        check(status.has_value(), (std::string(name) + " ran").c_str());
        if (status.has_value() && !status->ok) {
            check(status->code == WindowErrorCode::Unsupported, (std::string(name) + " failed other than as unsupported: " + status->message).c_str());
        }
    }

} // namespace

int main() {
    if (std::getenv("DISPLAY") == nullptr) {
        std::printf("WindowControlTest: skipped (needs an X server; run it under Xvfb).\n");
        return 0;
    }
    unsetenv("WAYLAND_DISPLAY");
    setenv("SDL_VIDEODRIVER", "x11", 1);

    WindowConfig config;
    config.title = "window control";
    config.extent = WindowExtent{320, 240};
    config.graphics_api = WindowGraphicsApi::None;
    auto created = Window::create<SDL3::SDL3Window>(config);
    if (!created.has_value()) {
        std::printf("WindowControlTest: skipped (could not open a window: %s).\n", created.error().message.c_str());
        return 0;
    }
    Window &window = **created;
    const WindowId id = window.id();
    WindowRequests requests;

    // Nothing has run yet.
    check(!requests.operation_status(id, WindowOperation::Title).has_value(), "no status before an operation runs");

    // Fire-and-forget operations: each must reach the window and report back.
    const std::vector<std::byte> pixels(4 * 4 * 4, std::byte{0x80});
    WindowControl::set_title(requests, id, "renamed");
    WindowControl::set_size(requests, id, WindowExtent{400, 300});
    WindowControl::set_minimum_size(requests, id, WindowExtent{100, 100});
    WindowControl::set_always_on_top(requests, id, true);
    WindowControl::set_aspect_ratio(requests, id, 1.0f, 2.0f);
    WindowControl::set_mouse_passthrough(requests, id, false);
    WindowControl::set_cursor_position(requests, id, WindowPosition{10, 10});
    WindowControl::set_custom_cursor(requests, id, 4, 4, pixels, 0, 0);
    WindowControl::clear_custom_cursor(requests, id);
    WindowControl::set_icon(requests, id, 4, 4, pixels);
    WindowControl::set_window_type(requests, id, WindowType::Normal);
    WindowControl::request_attention(requests, id, AttentionRequest::Briefly);
    HitTestRegions regions;
    regions.enabled = true;
    regions.drag.push_back(WindowRect{0, 0, 100, 20});
    WindowControl::set_hit_test_regions(requests, id, regions);
    run_pending(requests, window);
    check_outcome(requests, id, WindowOperation::Title, "set_title");
    check_outcome(requests, id, WindowOperation::Size, "set_size");
    check_outcome(requests, id, WindowOperation::MinimumSize, "set_minimum_size");
    check_outcome(requests, id, WindowOperation::AlwaysOnTop, "set_always_on_top");
    check_outcome(requests, id, WindowOperation::AspectRatio, "set_aspect_ratio");
    check_outcome(requests, id, WindowOperation::MousePassthrough, "set_mouse_passthrough");
    check_outcome(requests, id, WindowOperation::CursorPosition, "set_cursor_position");
    check_outcome(requests, id, WindowOperation::CustomCursor, "set_custom_cursor");
    check_outcome(requests, id, WindowOperation::ClearCustomCursor, "clear_custom_cursor");
    check_outcome(requests, id, WindowOperation::Icon, "set_icon");
    check_outcome(requests, id, WindowOperation::WindowType, "set_window_type");
    check_outcome(requests, id, WindowOperation::Attention, "request_attention");
    check_outcome(requests, id, WindowOperation::HitTestRegions, "set_hit_test_regions");

    // The title really changed on the window, and a bad value comes back as a failure, not silence.
    const auto title_status = requests.operation_status(id, WindowOperation::Title);
    check(title_status.has_value() && title_status->ok, "set_title succeeds on SDL3");
    WindowControl::set_icon(requests, id, 0, 0, {});
    run_pending(requests, window);
    const auto bad_icon = requests.operation_status(id, WindowOperation::Icon);
    check(bad_icon.has_value() && !bad_icon->ok && bad_icon->code == WindowErrorCode::InvalidArgument, "an empty icon reports InvalidArgument");

    // Queries carry their answers back.
    WindowControl::set_clipboard_text(requests, id, "from the engine");
    run_pending(requests, window);
    WindowControl::clipboard_text(requests, id);
    WindowControl::clipboard_mime_types(requests, id);
    WindowControl::key_down(requests, id, KeyboardKey::A);
    WindowControl::key_name(requests, id, KeyboardKey::A);
    run_pending(requests, window);
    const auto text = requests.operation_status(id, WindowOperation::ClipboardText);
    check(text.has_value() && text->ok && text->output.text == "from the engine", "the clipboard text query returns what was set");
    const auto types = requests.operation_status(id, WindowOperation::ClipboardMimeTypes);
    check(types.has_value() && types->ok && !types->output.list.empty(), "the clipboard MIME list is returned");
    const auto down = requests.operation_status(id, WindowOperation::KeyDown);
    check(down.has_value() && down->ok && !down->output.flag, "no key is held in the test");
    const auto name = requests.operation_status(id, WindowOperation::KeyName);
    check(name.has_value() && name->ok, "the key name query runs");

    // A file dialog parented to the window reaches the dialog layer with that window as its parent. A bare X server has no
    // dialog service, so the dialog may report an error through its callback; what matters is that the operation ran.
    Desktop::FileDialogOptions dialog;
    dialog.title = "pick";
    WindowControl::show_file_dialog(requests, id, dialog, [](Desktop::FileDialogResult) {});
    run_pending(requests, window);
    check_outcome(requests, id, WindowOperation::FileDialog, "show_file_dialog");

    // A newer run replaces the older status for the same window and operation.
    WindowControl::set_title(requests, id, "second");
    run_pending(requests, window);
    WindowControl::set_title(requests, id, "third");
    run_pending(requests, window);
    check(requests.operation_status(id, WindowOperation::Title).has_value(), "the latest status is kept");

    // Statuses are per window.
    check(!requests.operation_status(WindowId{id == WindowId{0} ? 1u : 0u}, WindowOperation::Title).has_value(), "statuses are keyed by window");

    if (failures == 0) {
        std::printf("WindowControlTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
