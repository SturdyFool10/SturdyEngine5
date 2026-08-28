#include <FFI/AbiSupport.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace SFT::Ffi {

    namespace {

        /// The calling thread's most recent error detail.
        ///
        /// Thread-local rather than process-wide because the engine renders windows on dedicated
        /// per-window threads: a shared slot would let one thread's failure overwrite the message
        /// another thread is about to read.
        thread_local std::string g_last_error_message;

        struct HandleEntry {
            HandleKind kind;
            void *pointer;
        };

        /// Guards `g_handles` and `g_next_token`.
        ///
        /// A plain mutex rather than anything lock-free: handles are minted twice per frame per
        /// window, which is nowhere near contended enough to justify the complexity.
        std::mutex g_handle_mutex;
        std::unordered_map<u64, HandleEntry> g_handles;

        /// Next token to hand out. Starts at 1 so 0 is always an invalid token, which makes a
        /// zero-initialized `SturdyEngine`/`SturdyFrame` — the shape a caller gets from `calloc`
        /// or a default-constructed binding struct — reliably rejected instead of ambiguous.
        u64 g_next_token = 1;

    } // namespace

    SturdyResult set_error(SturdyResult result, std::string_view message) noexcept {
        try {
            g_last_error_message.assign(message);
        } catch (...) {
            // Reporting the error must never itself fail the call. A caller that gets an empty
            // detail string still gets the correct SturdyResult, which is the part it branches on.
            g_last_error_message.clear();
        }
        return result;
    }

    void clear_error() noexcept {
        g_last_error_message.clear();
    }

    u64 mint_handle(HandleKind kind, void *pointer) {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};
        const u64 token = g_next_token++;
        g_handles.emplace(token, HandleEntry{kind, pointer});
        return token;
    }

    void revoke_handle(u64 token) noexcept {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};
        g_handles.erase(token);
    }

    SturdyResult resolve_handle(u64 token, HandleKind kind, void **out_pointer) noexcept {
        const std::lock_guard<std::mutex> lock{g_handle_mutex};

        const auto entry = g_handles.find(token);
        if (entry == g_handles.end()) {
            // Tokens are handed out in increasing order and never reused, so anything below the
            // counter was minted at some point and has since been revoked. That distinction is
            // worth reporting: "expired" tells a caller it kept a handle past its callback, while
            // "invalid" tells it the value never came from this ABI at all.
            if (token != 0 && token < g_next_token) {
                return set_error(STURDY_ERROR_HANDLE_EXPIRED,
                                 "handle used outside the callback that provided it");
            }
            return set_error(STURDY_ERROR_INVALID_HANDLE, "handle was never valid");
        }

        if (entry->second.kind != kind) {
            return set_error(STURDY_ERROR_INVALID_HANDLE, "handle refers to a different object kind");
        }

        *out_pointer = entry->second.pointer;
        return STURDY_OK;
    }

    SturdyResult resolve_engine(SturdyEngine engine, Engine::Engine **out_engine) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(engine.token, HandleKind::Engine, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_engine = static_cast<Engine::Engine *>(pointer);
        return STURDY_OK;
    }

    SturdyResult copy_string_out(std::string_view text,
                                 char *buffer,
                                 usize capacity,
                                 usize *out_length) noexcept {
        const usize required = text.size() + 1;
        if (out_length != nullptr) {
            *out_length = required;
        }
        if (buffer == nullptr || capacity == 0) {
            // A size query is not a failure when the caller asked for one, but a caller that
            // passed a real buffer of zero capacity did not get its string.
            return buffer == nullptr ? STURDY_OK : STURDY_ERROR_BUFFER_TOO_SMALL;
        }

        const usize copied = std::min(text.size(), capacity - 1);
        std::memcpy(buffer, text.data(), copied);
        buffer[copied] = '\0';
        if (copied != text.size()) {
            return set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "the supplied text buffer is too small");
        }
        return STURDY_OK;
    }

} // namespace SFT::Ffi

extern "C" {

const char *STURDY_ABI_CALL sturdy_last_error_message(void) {
    // Never null, even before any failure: `std::string::c_str()` on an empty string returns a
    // valid pointer to a terminator, which is exactly the "empty means no error" contract.
    return SFT::Ffi::g_last_error_message.c_str();
}

uint32_t STURDY_ABI_CALL sturdy_abi_version_major(void) {
    return STURDY_ABI_VERSION_MAJOR;
}

uint32_t STURDY_ABI_CALL sturdy_abi_version_minor(void) {
    return STURDY_ABI_VERSION_MINOR;
}

} // extern "C"
