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

    /// A machine-readable second opinion on `code`, for failures a program can act on rather than just report.
    enum class IoErrorReason {
        None,
        /// A server could not bind a port below the operating system's unprivileged range (Linux: below
        /// `net.ipv4.ip_unprivileged_port_start`, 1024 by default, unless the process is root or holds `CAP_NET_BIND_SERVICE`).
        /// `code` is `PermissionDenied`. Tailor to it: take the port from configuration with a high default, and show `remedy`.
        PrivilegedPort,
    };

    struct IoError {
        IoErrorCode code;
        /// What happened, including (for errors with a `reason`) why and how to get around it, so a log line or a foreign-language
        /// caller that only sees this string still gets the whole story.
        std::string message;
        IoErrorReason reason = IoErrorReason::None;
        /// The ways around the failure, one per line, ready to show to a user or an operator; empty when there is nothing to suggest.
        std::string remedy;

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
