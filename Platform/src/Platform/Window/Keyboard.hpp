#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Platform::Windowing {

    /// Provider-neutral key identity. WindowKeyboardEvent retains each backend's raw key and
    /// scancode alongside this value for consumers that need provider- or layout-specific data.
    enum class KeyboardKey : i32 {
        Unknown = 0,

        Backspace = '\b',
        Tab = '\t',
        Enter = '\r',
        Space = ' ',
        Apostrophe = '\'',
        Comma = ',',
        Minus = '-',
        Period = '.',
        Slash = '/',
        Digit0 = '0',
        Digit1 = '1',
        Digit2 = '2',
        Digit3 = '3',
        Digit4 = '4',
        Digit5 = '5',
        Digit6 = '6',
        Digit7 = '7',
        Digit8 = '8',
        Digit9 = '9',
        Semicolon = ';',
        Equal = '=',
        A = 'a',
        B = 'b',
        C = 'c',
        D = 'd',
        E = 'e',
        F = 'f',
        G = 'g',
        H = 'h',
        I = 'i',
        J = 'j',
        K = 'k',
        L = 'l',
        M = 'm',
        N = 'n',
        O = 'o',
        P = 'p',
        Q = 'q',
        R = 'r',
        S = 's',
        T = 't',
        U = 'u',
        V = 'v',
        W = 'w',
        X = 'x',
        Y = 'y',
        Z = 'z',
        LeftBracket = '[',
        Backslash = '\\',
        RightBracket = ']',
        GraveAccent = '`',

        Escape = 0x100,
        Insert,
        Delete,
        Right,
        Left,
        Down,
        Up,
        PageUp,
        PageDown,
        Home,
        End,
        CapsLock,
        ScrollLock,
        NumLock,
        PrintScreen,
        Pause,
        LeftShift,
        LeftControl,
        LeftAlt,
        LeftSuper,
        RightShift,
        RightControl,
        RightAlt,
        RightSuper,
        Menu,

        /// New blocks get their own base offset (well clear of the ~25-entry 0x100 block above) rather
        /// than continuing that sequence, so future insertions into either block never renumber this one.
        F1 = 0x200,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        F13,
        F14,
        F15,
        F16,
        F17,
        F18,
        F19,
        F20,
        F21,
        F22,
        F23,
        F24,


        Numpad0 = 0x300,
        Numpad1,
        Numpad2,
        Numpad3,
        Numpad4,
        Numpad5,
        Numpad6,
        Numpad7,
        Numpad8,
        Numpad9,
        NumpadDecimal,
        NumpadDivide,
        NumpadMultiply,
        NumpadSubtract,
        NumpadAdd,
        NumpadEnter,
        NumpadEqual,

        /// GLFW has no media-key constants at all (checked its full keycode table) — these stay
        /// KeyboardKey::Unknown on that backend. Same honest architecture-limited-coverage stance
        /// already used elsewhere in this codebase (e.g. Foundation::Cpu's Arm SVE detection, always
        /// false there for the same "the backend genuinely cannot report this" reason).
        VolumeUp = 0x400,
        VolumeDown,
        Mute,
        MediaPlayPause,
        MediaNext,
        MediaPrevious,
        MediaStop,
    };

    [[nodiscard]] constexpr KeyboardKey keyboard_key_from_ascii(i32 key) noexcept {
        if (key >= 'A' && key <= 'Z') {
            key += 'a' - 'A';
        }
        if ((key >= '0' && key <= '9') || (key >= 'a' && key <= 'z')) {
            return static_cast<KeyboardKey>(key);
        }

        switch (key) {
            case '\b': return KeyboardKey::Backspace;
            case '\t': return KeyboardKey::Tab;
            case '\r': return KeyboardKey::Enter;
            case ' ': return KeyboardKey::Space;
            case '\'': return KeyboardKey::Apostrophe;
            case ',': return KeyboardKey::Comma;
            case '-': return KeyboardKey::Minus;
            case '.': return KeyboardKey::Period;
            case '/': return KeyboardKey::Slash;
            case ';': return KeyboardKey::Semicolon;
            case '=': return KeyboardKey::Equal;
            case '[': return KeyboardKey::LeftBracket;
            case '\\': return KeyboardKey::Backslash;
            case ']': return KeyboardKey::RightBracket;
            case '`': return KeyboardKey::GraveAccent;
            default: return KeyboardKey::Unknown;
        }
    }

    [[nodiscard]] constexpr bool is_modifier_key(KeyboardKey key) noexcept {
        switch (key) {
            case KeyboardKey::LeftShift:
            case KeyboardKey::LeftControl:
            case KeyboardKey::LeftAlt:
            case KeyboardKey::LeftSuper:
            case KeyboardKey::RightShift:
            case KeyboardKey::RightControl:
            case KeyboardKey::RightAlt:
            case KeyboardKey::RightSuper:
                return true;
            default:
                return false;
        }
    }

    /// Normalized modifier bitmask — WindowKeyboardEvent::modifiers carries this (SDL3/GLFW backends
    /// translate their own native modifier bits into it), so a consumer never has to know whether an
    /// event came from SDL's SDL_Keymod or GLFW's mods bitfield to check "is Shift held." Left/right
    /// side is deliberately *not* distinguished here (that's what the left/right KeyboardKey
    /// enumerators above are for, via is_modifier_key()/direct key_down() checks) — this is the
    /// coarse "any Shift" convenience the way every other engine's modifier mask works.
    enum class KeyModifiers : u32 {
        None = 0,
        Shift = 1u << 0,
        Control = 1u << 1,
        Alt = 1u << 2,
        Super = 1u << 3,
        CapsLock = 1u << 4,
        NumLock = 1u << 5,
    };

    [[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept {
        return static_cast<KeyModifiers>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    [[nodiscard]] constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) noexcept {
        return static_cast<KeyModifiers>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    [[nodiscard]] constexpr KeyModifiers operator~(KeyModifiers a) noexcept {
        return static_cast<KeyModifiers>(~static_cast<u32>(a));
    }

    constexpr KeyModifiers &operator|=(KeyModifiers &a, KeyModifiers b) noexcept { return a = a | b; }
    constexpr KeyModifiers &operator&=(KeyModifiers &a, KeyModifiers b) noexcept { return a = a & b; }

    [[nodiscard]] constexpr bool has_modifier(KeyModifiers value, KeyModifiers flag) noexcept {
        return (value & flag) != KeyModifiers::None;
    }

    /// The modifier a given left/right KeyboardKey contributes to the coarse KeyModifiers mask, or
    /// KeyModifiers::None for every non-modifier key. Backends use this to fold their per-side key
    /// state into one mask; InputState (Engine/src/Engine/InputState.hpp) reuses it the same way when
    /// deriving modifiers() from the raw pressed-key set rather than trusting a second, driftable
    /// source of truth.
    [[nodiscard]] constexpr KeyModifiers modifier_for_key(KeyboardKey key) noexcept {
        switch (key) {
            case KeyboardKey::LeftShift:
            case KeyboardKey::RightShift:
                return KeyModifiers::Shift;
            case KeyboardKey::LeftControl:
            case KeyboardKey::RightControl:
                return KeyModifiers::Control;
            case KeyboardKey::LeftAlt:
            case KeyboardKey::RightAlt:
                return KeyModifiers::Alt;
            case KeyboardKey::LeftSuper:
            case KeyboardKey::RightSuper:
                return KeyModifiers::Super;
            case KeyboardKey::CapsLock:
                return KeyModifiers::CapsLock;
            case KeyboardKey::NumLock:
                return KeyModifiers::NumLock;
            default:
                return KeyModifiers::None;
        }
    }

} // namespace SFT::Platform::Windowing
