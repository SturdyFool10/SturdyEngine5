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
        WindowPosition position = {};
        WindowResize resize = {};
        WindowKeyboardEvent keyboard = {};
        WindowTextInputEvent text = {};
        WindowMouseMoveEvent mouse_move = {};
        WindowMouseButtonEvent mouse_button = {};
        WindowMouseWheelEvent mouse_wheel = {};
    };

} // namespace SFT::Platform::Windowing
