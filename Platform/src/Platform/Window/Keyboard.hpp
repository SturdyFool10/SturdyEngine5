#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Platform::Windowing {

    // Provider-neutral key identity. WindowKeyboardEvent retains each backend's raw key and
    // scancode alongside this value for consumers that need provider- or layout-specific data.
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

} // namespace SFT::Platform::Windowing
