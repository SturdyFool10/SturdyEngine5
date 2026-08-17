#include "WindowTextUtil.hpp"

#include <cstring>

namespace SFT::Platform::Windowing {

    void copy_utf8_truncated(char *dest, usize dest_capacity, const char *src) noexcept {
        if (dest_capacity == 0) {
            return;
        }
        usize copy_size = strnlen(src, dest_capacity - 1);
        // A UTF-8 continuation byte is 10xxxxxx; back off until `copy_size` either lands on a
        // sequence-start byte (or ASCII) or hits zero, so the copied prefix is always well-formed
        // UTF-8 on its own.
        while (copy_size > 0 && (static_cast<unsigned char>(src[copy_size]) & 0xC0) == 0x80) {
            --copy_size;
        }
        memcpy(dest, src, copy_size);
        dest[copy_size] = '\0';
    }

} // namespace SFT::Platform::Windowing
