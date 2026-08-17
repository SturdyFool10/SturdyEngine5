#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::Platform::Windowing {

    // Copies as much of `src` (NUL-terminated UTF-8) as fits in `dest_capacity` bytes (including the
    // trailing NUL this always writes), backing off from the end of `src` one byte at a time if a
    // straight length-based cut would land inside a multi-byte UTF-8 sequence — a plain strnlen()+
    // memcpy() truncation can otherwise split a CJK/emoji codepoint in half, corrupting the last
    // character of an over-long IME composition instead of just dropping it. `dest` need not be
    // zero-initialized; always NUL-terminates. Shared by every window provider that copies IME
    // composition/committed text into a WindowTextInputEvent/WindowTextEditingEvent's fixed buffer
    // (Platform's own SDL3 backend, GlfwWindowProvider's native IME hooks) so the same truncation
    // correctness doesn't have to be re-derived per provider.
    void copy_utf8_truncated(char *dest, usize dest_capacity, const char *src) noexcept;

} // namespace SFT::Platform::Windowing
