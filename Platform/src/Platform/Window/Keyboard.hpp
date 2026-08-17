#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Platform::Windowing {


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


        VolumeUp = 0x400,
        VolumeDown,
        Mute,
        MediaPlayPause,
        MediaNext,
        MediaPrevious,
        MediaStop,
    };

    /// Performs the keyboard key from ascii operation using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Reports whether modifier key holds.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
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


    enum class KeyModifiers : u32 {
        None = 0,
        Shift = 1u << 0,
        Control = 1u << 1,
        Alt = 1u << 2,
        Super = 1u << 3,
        CapsLock = 1u << 4,
        NumLock = 1u << 5,
    };

    /// Combines the operands with bitwise OR.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept {
        return static_cast<KeyModifiers>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// Combines the operands with bitwise AND.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) noexcept {
        return static_cast<KeyModifiers>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// Implements `operator~` for `Windowing`.
    ///
    /// @param a `a` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr KeyModifiers operator~(KeyModifiers a) noexcept {
        return static_cast<KeyModifiers>(~static_cast<u32>(a));
    }

    /// Combines this object with the right-hand operand using bitwise OR.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    constexpr KeyModifiers &operator|=(KeyModifiers &a, KeyModifiers b) noexcept { return a = a | b; }
    /// Combines this object with the right-hand operand using bitwise AND.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    constexpr KeyModifiers &operator&=(KeyModifiers &a, KeyModifiers b) noexcept { return a = a & b; }

    /// Reports whether modifier is available.
    ///
    /// @param value Value consumed by the operation.
    /// @param flag `flag` value used by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool has_modifier(KeyModifiers value, KeyModifiers flag) noexcept {
        return (value & flag) != KeyModifiers::None;
    }


    /// Performs the modifier for key operation for `Windowing` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
