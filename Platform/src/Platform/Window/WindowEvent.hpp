#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Platform/Window/Keyboard.hpp>
#include <Platform/Window/WindowGeometry.hpp>

namespace SFT::Platform::Windowing {

    struct WindowResize {
        WindowExtent previous = {};
        WindowExtent current = {};
        WindowExtent framebuffer = {};
        bool framebuffer_changed = false;
    };

    enum class WindowEventKind {
        CloseRequested,
        Moved,
        Resized,
        FramebufferResized,
        FocusGained,
        FocusLost,
        MouseEntered,
        MouseLeft,
        KeyPressed,
        KeyReleased,
        TextInput,
        MouseMoved,
        MouseButtonPressed,
        MouseButtonReleased,
        MouseWheel,
        MouseLocked,
        MouseUnlocked,
    };

    struct WindowKeyboardEvent {
        // Lossless provider-native values remain available for backend-specific integrations.
        i32 key = 0;
        i32 scancode = 0;
        u32 modifiers = 0;
        bool repeat = false;

        // Stable identity shared by every window provider. Unlike modifiers, this identifies the
        // modifier key that generated an event, including its left/right side.
        KeyboardKey key_code = KeyboardKey::Unknown;
    };

    struct WindowTextInputEvent {
        char utf8[32] = {};
    };

    struct WindowMouseMoveEvent {
        f32 x = 0.0F;
        f32 y = 0.0F;
        f32 delta_x = 0.0F;
        f32 delta_y = 0.0F;
        u32 buttons = 0;
    };

    // Stable mouse-button identity shared by every provider. Provider-native numbering is not
    // consistent (SDL uses 1 for left while GLFW uses 0), so consumers should prefer this over the
    // raw WindowMouseButtonEvent::button value whenever they do not need backend-specific details.
    // Extra1..Extra12 covers real high-button-count gaming mice (e.g. a 12-side-button mouse) — SDL3
    // reports raw button indices past X1/X2 as plain numbers with no further naming, so this is this
    // engine's own extended numbering, not an SDL/GLFW one. GLFW itself caps at 8 total buttons
    // (GLFW_MOUSE_BUTTON_LAST == GLFW_MOUSE_BUTTON_8), so only Extra1..Extra5 are ever reachable on
    // that backend — same architecture-limited-coverage stance as KeyboardKey's media keys.
    enum class MouseButton : u8 {
        Unknown,
        Left,
        Middle,
        Right,
        Extra1,
        Extra2,
        Extra3,
        Extra4,
        Extra5,
        Extra6,
        Extra7,
        Extra8,
        Extra9,
        Extra10,
        Extra11,
        Extra12,
    };

    struct WindowMouseButtonEvent {
        // Lossless provider-native value retained for backend-specific integrations and source
        // compatibility with the original event contract.
        u8 button = 0;
        u8 clicks = 1;
        f32 x = 0.0F;
        f32 y = 0.0F;

        // Appended so existing four-field aggregate initialization remains source-compatible.
        MouseButton button_code = MouseButton::Unknown;
    };

    struct WindowMouseWheelEvent {
        f32 x = 0.0F;
        f32 y = 0.0F;
        f32 mouse_x = 0.0F;
        f32 mouse_y = 0.0F;
    };

    struct WindowEvent {
        WindowEventKind kind = WindowEventKind::CloseRequested;

        // Monotonic capture time in nanoseconds (std::chrono::steady_clock epoch), used for
        // input-to-consumption latency measurement. Populated as close to the original hardware/OS
        // delivery as each backend allows: SDL3 events carry their own SDL_GetTicksNS()-based
        // timestamp, which is converted to steady_clock's epoch once per pump_events() call; GLFW has
        // no native per-event timestamp, so its callbacks stamp steady_clock::now() at the moment the
        // callback fires. Synthetic engine-initiated events (request_close(), set_mouse_locked()) also
        // stamp steady_clock::now() since they have no originating device event to inherit from.
        u64 timestamp_ns = 0;

        WindowPosition position = {};
        WindowResize resize = {};
        WindowKeyboardEvent keyboard = {};
        WindowTextInputEvent text = {};
        WindowMouseMoveEvent mouse_move = {};
        WindowMouseButtonEvent mouse_button = {};
        WindowMouseWheelEvent mouse_wheel = {};
    };

} // namespace SFT::Platform::Windowing
