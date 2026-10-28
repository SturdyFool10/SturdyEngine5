#pragma once

#include <string>
#include <string_view>

namespace SFT::Async {

    enum class IoErrorCode {
        NotFound,
        PermissionDenied,
        AlreadyExists,
        InvalidArgument,
        ConnectionRefused,
        ConnectionReset,
        TimedOut,
        Unsupported,
        Unknown,
        /// The peer closed the stream before a fixed-size read (`receive_exact`) was satisfied.
        UnexpectedEof,
        /// The local object was closed (or never opened) before/while the operation ran.
        Closed,
        /// TLS handshake or certificate/pin verification failed, or the TLS session broke.
        TlsFailure,
        /// A configured size/count limit was exceeded (message, frame, header, body, queue).
        LimitExceeded,
        /// The peer violated the protocol (bad HTTP/WebSocket framing, handshake mismatch).
        ProtocolError,
    };

    struct IoError {
        IoErrorCode code;
        std::string message;

        /// Constructs a `IoError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        IoError(IoErrorCode error_code, const char *error_message) noexcept;

        /// Constructs a `IoError` from the supplied initialization values.
        ///
        /// @param error_code `error_code` value used by the operation.
        /// @param error_message `error_message` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        IoError(IoErrorCode error_code, std::string_view error_message) noexcept;
    };

} // namespace SFT::Async
