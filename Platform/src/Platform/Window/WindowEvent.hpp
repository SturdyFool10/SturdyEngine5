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
        TextEditing,
        MouseMoved,
        MouseButtonPressed,
        MouseButtonReleased,
        MouseWheel,
        MouseLocked,
        MouseUnlocked,
    };

    struct WindowKeyboardEvent {
        /// Lossless provider-native values remain available for backend-specific integrations.
        i32 key = 0;
        i32 scancode = 0;
        u32 modifiers = 0;
        bool repeat = false;

        /// Stable identity shared by every window provider. Unlike modifiers, this identifies the
        /// modifier key that generated an event, including its left/right side.
        KeyboardKey key_code = KeyboardKey::Unknown;
    };

    struct WindowTextInputEvent {
        char utf8[32] = {};
    };

    /// An in-progress IME composition update (the underlined "preedit" text a romaji-to-hiragana,
    /// pinyin, or similar IME shows before the user confirms it) — SDL3's SDL_EVENT_TEXT_EDITING;
    /// GLFW has no equivalent (stock GLFW composes invisibly inside its own platform backend and
    /// only ever surfaces the final committed text via its char callback, so this event never fires
    /// on the GLFW backend). SDL3's own SDL_TextEditingEvent::text is an unbounded `const char *`
    /// (SDL itself never truncates), so the only truncation risk is this struct's own mirror of it —
    /// 512 bytes (~170 CJK codepoints at 3 bytes each) comfortably covers realistic compositions (a
    /// long compound word typed before pressing space to convert, or a long Pinyin sequence before a
    /// candidate is picked) without switching this to a dynamically-sized string, which WindowEvent
    /// deliberately avoids: it stays a flat, trivially-copyable aggregate consumed through
    /// WindowManager's coalescing ring buffer (up to thousands of queued events for an 8kHz-class
    /// input device), where a heap-allocated field on every event would be a real cost. A composition
    /// that still somehow exceeds 512 bytes is truncated at a UTF-8 codepoint boundary (never
    /// mid-sequence) by the window provider, not silently dropped or corrupted.
    ///
    /// Per SDL3's own documented contract, an **empty** `utf8` means composition has ended (either
    /// confirmed — a WindowTextInputEvent with the final text follows — or cancelled) and nothing
    /// should be shown; do not infer "still composing" from anything other than utf8 being non-empty.
    struct WindowTextEditingEvent {
        char utf8[512] = {};
        /// Caret position within `utf8`, and the length of the currently-selected span within it —
        /// both as reported by the backend, `-1` for either when the backend doesn't set it. Optional
        /// to use: a consumer that just wants to show the whole composition string underlined at the
        /// widget's own caret (the common case) can ignore both and only check whether `utf8` is empty.
        i32 cursor = 0;
        i32 selection_length = 0;
    };

    struct WindowMouseMoveEvent {
        f32 x = 0.0F;
        f32 y = 0.0F;
        f32 delta_x = 0.0F;
        f32 delta_y = 0.0F;
        u32 buttons = 0;
    };

    /// Stable mouse-button identity shared by every provider. Provider-native numbering is not
    /// consistent (SDL uses 1 for left while GLFW uses 0), so consumers should prefer this over the
    /// raw WindowMouseButtonEvent::button value whenever they do not need backend-specific details.
    /// Extra1..Extra12 covers real high-button-count gaming mice (e.g. a 12-side-button mouse) — SDL3
    /// reports raw button indices past X1/X2 as plain numbers with no further naming, so this is this
    /// engine's own extended numbering, not an SDL/GLFW one. GLFW itself caps at 8 total buttons
    /// (GLFW_MOUSE_BUTTON_LAST == GLFW_MOUSE_BUTTON_8), so only Extra1..Extra5 are ever reachable on
    /// that backend — same architecture-limited-coverage stance as KeyboardKey's media keys.
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
        /// Lossless provider-native value retained for backend-specific integrations and source
        /// compatibility with the original event contract.
        u8 button = 0;
        u8 clicks = 1;
        f32 x = 0.0F;
        f32 y = 0.0F;

        /// Appended so existing four-field aggregate initialization remains source-compatible.
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

        /// Monotonic capture time in nanoseconds (std::chrono::steady_clock epoch), used for
        /// input-to-consumption latency measurement. Populated as close to the original hardware/OS
        /// delivery as each backend allows: SDL3 events carry their own SDL_GetTicksNS()-based
        /// timestamp, which is converted to steady_clock's epoch once per pump_events() call; GLFW has
        /// no native per-event timestamp, so its callbacks stamp steady_clock::now() at the moment the
        /// callback fires. Synthetic engine-initiated events (request_close(), set_mouse_locked()) also
        /// stamp steady_clock::now() since they have no originating device event to inherit from.
        u64 timestamp_ns = 0;

        WindowPosition position = {};
        WindowResize resize = {};
        WindowKeyboardEvent keyboard = {};
        WindowTextInputEvent text = {};
        WindowTextEditingEvent editing = {};
        WindowMouseMoveEvent mouse_move = {};
        WindowMouseButtonEvent mouse_button = {};
        WindowMouseWheelEvent mouse_wheel = {};
    };

} // namespace SFT::Platform::Windowing
