#pragma once

/// Private helpers shared by the C ABI files that translate `Async/Binding.hpp`: the handle tables for operations and objects, the
/// firewall that maps facade errors to result codes, and the conversions between the C option structs and the facade's plain types.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Async/Binding.hpp>

#include <FFI/AbiSupport.hpp>

namespace B = SFT::Async::Binding;

namespace SFT::Ffi::AsyncSupport {

    using SFT::Ffi::guarded;
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u64;

    struct OpEntry {
        B::OpPtr op;
        u64 object_token = 0; ///< the handle `sturdy_op_object` minted, so repeated calls agree
    };

    /// One lock for every table below; held only for map operations, never while calling into the facade.
    extern std::mutex g_mutex;
    /// The tables are deliberately leaked: objects still alive at process exit (a child process, a socket) own Asio state that must not be
    /// destroyed after the network reactor itself has been torn down by static destruction.
    extern std::unordered_map<u64, std::unique_ptr<OpEntry>> &g_ops;
    extern std::unordered_map<u64, std::unique_ptr<B::ObjectPtr>> &g_objects;
    extern std::unordered_map<u64, std::unique_ptr<B::HttpRespond>> &g_responders;

    [[nodiscard]] inline SturdyResult map_code(SFT::Async::IoErrorCode code) noexcept {
        using C = SFT::Async::IoErrorCode;
        switch (code) {
            case C::InvalidArgument: return STURDY_ERROR_INVALID_ARGUMENT;
            case C::Unsupported: return STURDY_ERROR_NOT_AVAILABLE;
            case C::LimitExceeded: return STURDY_ERROR_OUT_OF_RANGE;
            default: return STURDY_ERROR_INITIALIZATION_FAILED;
        }
    }

    /// Runs `body` behind the firewall, turning facade misuse errors into the ABI's result codes.
    template <typename Body>
    [[nodiscard]] SturdyResult run(Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            try {
                return body();
            } catch (const B::BindingError &error) {
                return set_error(map_code(error.code()), error.what());
            }
        });
    }

    [[nodiscard]] SturdyResult publish_op(B::OpPtr op, SturdyAsyncOp *out);
    [[nodiscard]] u64 publish_object_token(B::ObjectPtr object);
    /// A new object handle, or the zero handle for a null object.
    [[nodiscard]] SturdyAsyncObject publish_object(B::ObjectPtr object);
    [[nodiscard]] SturdyResult resolve_op(SturdyAsyncOp handle, B::OpPtr *out, u64 *object_token = nullptr) noexcept;
    [[nodiscard]] SturdyResult resolve_object(SturdyAsyncObject handle, B::ObjectPtr *out) noexcept;
    /// A finished operation holding `fields` (for the functions that return a snapshot).
    [[nodiscard]] SturdyResult publish_fields(B::Fields fields, SturdyAsyncOp *out);
    /// A finished operation holding `bytes`.
    [[nodiscard]] SturdyResult publish_bytes(B::Bytes bytes, SturdyAsyncOp *out);
    /// A finished operation holding `number`.
    [[nodiscard]] SturdyResult publish_number(std::int64_t number, SturdyAsyncOp *out);
    /// A finished operation holding `text`.
    [[nodiscard]] SturdyResult publish_text(std::string text, SturdyAsyncOp *out);

    [[nodiscard]] inline SturdyResult require_out(const void *pointer, const char *what) {
        if (pointer == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, what);
        return STURDY_OK;
    }

    [[nodiscard]] inline SturdyResult copy_bytes_out(const B::Bytes &data, uint8_t *buffer, size_t capacity, size_t *out_length) noexcept {
        if (out_length != nullptr) *out_length = data.size();
        if (buffer == nullptr) return STURDY_OK;
        const size_t count = (std::min)(capacity, data.size());
        if (count > 0) std::memcpy(buffer, data.data(), count);
        return count < data.size() ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "buffer is smaller than the result") : STURDY_OK;
    }

    // ---- C -> facade conversions ----
    inline std::string text(const char *s) { return s != nullptr ? std::string(s) : std::string{}; }
    inline std::vector<std::string> strings(const char *const *items, uint32_t count) {
        std::vector<std::string> out;
        if (items == nullptr) return out;
        for (uint32_t i = 0; i < count; ++i) out.push_back(text(items[i]));
        return out;
    }
    inline B::Fields fields_of(const SturdyKeyValue *items, uint32_t count) {
        B::Fields out;
        if (items == nullptr) return out;
        for (uint32_t i = 0; i < count; ++i) out.emplace_back(text(items[i].name), text(items[i].value));
        return out;
    }
    inline B::Bytes bytes_of(const uint8_t *data, size_t size) { return (data == nullptr || size == 0) ? B::Bytes{} : B::Bytes(data, data + size); }

    B::TlsClientSpec tls_of(const SturdyTlsClientOptions &o);
    B::TlsServerSpec tls_of(const SturdyTlsServerOptions &o);
    B::WebSocketSpec socket_of(const SturdyWebSocketOptions &o);
    B::QuicLimitsSpec limits_of(const SturdyQuicLimits &o);
    B::PortMapSpec portmap_of(const SturdyPortMapOptions *o);
    B::ProcessSpec process_of(const SturdyProcessOptions &o);

    inline SturdyIoError io_error_of(SFT::Async::IoErrorCode code) noexcept { return static_cast<SturdyIoError>(static_cast<int>(code) + 1); }

    /// Calls a foreign function that cannot be allowed to throw back into the engine.
    template <typename Fn>
    void no_throw(Fn &&fn) noexcept {
        try {
            fn();
        } catch (...) {
        }
    }

} // namespace SFT::Ffi::AsyncSupport

#define STURDY_OP_OUT(out_op)                                                                                                             \
    if (const auto bad_out = SFT::Ffi::AsyncSupport::require_out((out_op), "out_op must not be null"); bad_out != STURDY_OK) return bad_out

#define STURDY_WITH_OBJECT(handle, name)                                                                                                  \
    B::ObjectPtr name;                                                                                                                    \
    if (const auto resolved_object = SFT::Ffi::AsyncSupport::resolve_object((handle), &name); resolved_object != STURDY_OK) return resolved_object

#define STURDY_REQUIRE(pointer, what)                                                                                                     \
    if (const auto bad_pointer = SFT::Ffi::AsyncSupport::require_out((pointer), (what)); bad_pointer != STURDY_OK) return bad_pointer
