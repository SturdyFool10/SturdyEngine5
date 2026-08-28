#pragma once

/// Internal plumbing shared by every C ABI entry point: the exception firewall, the thread-local
/// error slot, and the scope-bound handle table.
///
/// Nothing here is part of the ABI. It exists so that `Sturdy.h`'s guarantees — no exception
/// escapes, no stale handle is dereferenced — are implemented once rather than restated in every
/// exported function.

#include <Foundation/Foundation.hpp>

#include <exception>
#include <new>
#include <string_view>
#include <utility>

#include <FFI/Sturdy.h>

namespace SFT::Engine {
    class Engine;
} // namespace SFT::Engine

namespace SFT::Ffi {

    /// Records `message` as the calling thread's most recent error and returns `result` so callers
    /// can `return set_error(...)` directly.
    ///
    /// @param result Status to return. Passing `STURDY_OK` is a contradiction and is never done;
    ///        use `clear_error()` for the success path.
    /// @param message Human-readable detail, copied into thread-local storage.
    ///
    /// @return `result`, unchanged.
    /// @note This function does not throw exceptions. If copying `message` would allocate and
    ///       fail, the stored detail is truncated or dropped rather than propagating.
    SturdyResult set_error(SturdyResult result, std::string_view message) noexcept;

    /// Clears the calling thread's error slot. Called on every successful path so a stale message
    /// from an earlier failure is never mistaken for detail about the current call.
    ///
    /// @note This function does not throw exceptions.
    void clear_error() noexcept;

    /// Kinds a handle token can refer to. Stored alongside every token so a `SturdyFrame` passed
    /// where a `SturdyEngine` is expected is rejected rather than reinterpreted — the two are
    /// structurally identical at the ABI, so this is the only thing that can tell them apart.
    enum class HandleKind : u32 {
        Engine = 1,
        Frame = 2,
        Commands = 3,
        /// Unlike the others, a scene handle is owned by the caller and lives until it is
        /// explicitly released rather than dying with a callback scope.
        GltfScene = 4,
        /// Also owned: a task outlives the call that spawned it.
        Task = 5,
    };

    /// Mints a token referring to `pointer`, valid until `revoke_handle`.
    ///
    /// Tokens come from a monotonically increasing 64-bit counter and are never reused. That is
    /// what makes a stale token safe: it can only ever resolve to "expired", never to a different
    /// object that happens to occupy the same slot.
    ///
    /// @param kind Kind the token refers to.
    /// @param pointer Borrowed object. Must outlive the matching `revoke_handle` call.
    ///
    /// @return The new token, never 0.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] u64 mint_handle(HandleKind kind, void *pointer);

    /// Invalidates `token`. Subsequent resolution reports `STURDY_ERROR_HANDLE_EXPIRED`.
    ///
    /// @param token Token previously returned by `mint_handle`. Unknown tokens are ignored.
    /// @note This function does not throw exceptions.
    void revoke_handle(u64 token) noexcept;

    /// Resolves `token` to the object it refers to.
    ///
    /// @param token Token to resolve.
    /// @param kind Kind the caller expects.
    /// @param out_pointer Receives the borrowed pointer on success.
    ///
    /// @return `STURDY_OK`; `STURDY_ERROR_HANDLE_EXPIRED` when the token was valid but its scope
    ///         has ended; `STURDY_ERROR_INVALID_HANDLE` when it was never minted or carries a
    ///         different kind.
    /// @note This function does not throw exceptions. The error slot is populated on failure.
    [[nodiscard]] SturdyResult resolve_handle(u64 token, HandleKind kind, void **out_pointer) noexcept;

    /// Resolves an engine handle to the engine it refers to.
    ///
    /// Shared by every `sturdy_engine_*`, `sturdy_rhi_*`, and `sturdy_native_*` entry point, so the
    /// scope and kind checks behave identically across all of them.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_engine Receives the borrowed engine on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions. The error slot is populated on failure.
    [[nodiscard]] SturdyResult resolve_engine(SturdyEngine engine, Engine::Engine **out_engine) noexcept;

    /// Registers the render-extraction systems a rendered frame depends on.
    ///
    /// The engine does not extract renderables on its own: an application registers systems that
    /// walk `WorldTransform` + `ModelRenderer` (and the light components) and submit them to the
    /// frame. A C++ product writes those itself; a foreign caller cannot, because they need typed
    /// component and resource access this ABI does not expose. Without them every entity is
    /// invisible while every call still reports success — so this runs once at startup, before any
    /// game-logic callback.
    ///
    /// @param engine Engine to configure.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void install_render_extraction(Engine::Engine &engine);

    /// Writes `text` into a caller-supplied buffer following the ABI's string-output convention.
    ///
    /// @param text Source text, not required to be null-terminated.
    /// @param buffer Destination, or null to query the required size.
    /// @param capacity Bytes available in `buffer`.
    /// @param out_length Receives the required size including the null terminator. May be null.
    ///
    /// @return `STURDY_OK`, or `STURDY_ERROR_BUFFER_TOO_SMALL` when `text` did not fit — in which
    ///         case `buffer` still receives a truncated but null-terminated result, so a caller
    ///         that ignores the status gets a short string rather than an unterminated one.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult copy_string_out(std::string_view text,
                                               char *buffer,
                                               usize capacity,
                                               usize *out_length) noexcept;

    /// Mints a handle for the duration of a scope and revokes it on the way out.
    ///
    /// This is what enforces `Sturdy.h`'s rule that a handle is only usable inside the callback
    /// that received it: the token dies with the C++ scope that owns the referenced object,
    /// whether the callback returned normally or the stack is unwinding.
    class ScopedHandle {
      public:
        /// Mints a token for `pointer`.
        ///
        /// @param kind Kind to mint.
        /// @param pointer Borrowed object, which must outlive this guard.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ScopedHandle(HandleKind kind, void *pointer) : token_(mint_handle(kind, pointer)) {}

        /// Revokes the token.
        ///
        /// @note This function does not throw exceptions.
        ~ScopedHandle() { revoke_handle(token_); }

        /// Disables this construction form for `ScopedHandle`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ScopedHandle(const ScopedHandle &) = delete;
        /// Assigns a new value to this `ScopedHandle`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        ScopedHandle &operator=(const ScopedHandle &) = delete;

        /// Returns the minted token.
        ///
        /// @return Returns the current token value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 token() const noexcept { return token_; }

      private:
        u64 token_;
    };

    /// Runs `body` behind the ABI's exception firewall.
    ///
    /// Every exported function routes through this. C++ exception unwinding has no defined
    /// behavior across a C ABI — and the Microsoft and Itanium ABIs disagree about its mechanics
    /// anyway — so nothing may escape, including from destructors running during unwind.
    ///
    /// @param body Callable returning `SturdyResult`.
    ///
    /// @return `body`'s result, or an error status describing the exception it raised.
    /// @note This function does not throw exceptions.
    template <typename Body>
    [[nodiscard]] SturdyResult guarded(Body &&body) noexcept {
        try {
            const SturdyResult result = std::forward<Body>(body)();
            if (result == STURDY_OK) {
                clear_error();
            }
            return result;
        } catch (const std::bad_alloc &) {
            return set_error(STURDY_ERROR_OUT_OF_MEMORY, "allocation failed");
        } catch (const std::exception &error) {
            return set_error(STURDY_ERROR_INTERNAL, error.what());
        } catch (...) {
            return set_error(STURDY_ERROR_INTERNAL,
                             "an unrecognized C++ exception reached the ABI boundary");
        }
    }

} // namespace SFT::Ffi
