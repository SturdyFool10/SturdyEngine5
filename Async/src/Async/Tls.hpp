#pragma once

#include <Async/IoError.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace SFT::Async {

    /// TLS settings for outgoing connections (TCP, WebSocket, HTTP client, peers).
    ///
    /// Safe by default: TLS 1.2+, the certificate chain and host name are always verified, and there is
    /// deliberately no "skip verification" switch. For private/self-signed servers either add the issuing
    /// CA (`ca_certificates_pem`) or pin the server's certificate (`pinned_sha256`).
    struct TlsClientOptions {
        /// Host name sent as SNI and checked against the certificate. Empty means "the host being connected to".
        std::string server_name;
        /// Trust the operating system's root store (in addition to `ca_certificates_pem`).
        bool use_system_roots = true;
        /// Extra trusted CA certificates, PEM text (may hold several).
        std::string ca_certificates_pem;
        /// Hex SHA-256 digests of acceptable leaf certificates (DER). When non-empty, the leaf must match
        /// one of them and ordinary chain/host validation is replaced by that exact-certificate check.
        std::vector<std::string> pinned_sha256;
        /// For peer-to-peer links where identity is a key, not a DNS name: accept whatever certificate the server
        /// presents (no CA or host-name validation) and let the caller decide from `TlsInfo::peer_sha256`. The TLS
        /// handshake still proves the server holds that certificate's private key, but nothing says the key belongs
        /// to anyone you expect -- so the caller MUST compare the fingerprint with an expected value (or knowingly
        /// trust it on first use). Ignored when `pinned_sha256` is set. Never on by default.
        bool identify_peer_by_certificate_only = false;
        /// Client certificate for mutual TLS (PEM chain + private key). Both or neither.
        std::string client_certificate_chain_pem;
        std::string client_private_key_pem;
        /// Application protocols to offer via ALPN, most preferred first (e.g. "h2", "http/1.1").
        std::vector<std::string> alpn;
        std::uint32_t handshake_timeout_ms = 10'000;
    };

    /// TLS settings for accepting connections.
    struct TlsServerOptions {
        /// Server certificate chain and private key, PEM text. Required.
        std::string certificate_chain_pem;
        std::string private_key_pem;
        /// CAs whose client certificates are accepted (mutual TLS). Empty means no client-cert validation.
        std::string client_ca_pem;
        /// Reject clients that present no (valid) certificate. Needs `client_ca_pem` or `pinned_client_sha256`.
        bool require_client_certificate = false;
        /// Require a client certificate but accept any self-signed one, exposing its fingerprint through
        /// `TlsInfo::peer_sha256` so the application can authorise by identity (peer-to-peer nodes). The client still
        /// proves possession of the matching private key. Without further checks this lets *anyone* connect; pair it
        /// with an allow-list.
        bool accept_any_client_certificate = false;
        /// Hex SHA-256 digests of acceptable client leaf certificates; when non-empty, the client must
        /// present one of these exact certificates (the P2P / self-signed-peer model).
        std::vector<std::string> pinned_client_sha256;
        /// Application protocols the server accepts via ALPN, most preferred first.
        std::vector<std::string> alpn;
        std::uint32_t handshake_timeout_ms = 10'000;
    };

    /// What was negotiated on a TLS connection.
    struct TlsInfo {
        std::string protocol;           ///< e.g. "TLSv1.3"
        std::string cipher;
        std::string alpn;               ///< selected application protocol, empty if none
        std::string peer_sha256;        ///< hex SHA-256 of the peer's leaf certificate, empty if it sent none
    };

    /// A freshly generated self-signed identity, for peers and dev servers. Generated with OpenSSL (ECDSA P-256).
    struct TlsIdentity {
        std::string certificate_pem;
        std::string private_key_pem;
        /// Hex SHA-256 of the certificate: share it out of band and pin it on the other side.
        std::string sha256;
    };

    [[nodiscard]] std::expected<TlsIdentity, IoError> generate_self_signed_identity(const std::string &common_name,
                                                                                    unsigned valid_days = 365);

    /// Hex SHA-256 of the first certificate in `certificate_pem`.
    [[nodiscard]] std::expected<std::string, IoError> certificate_sha256(const std::string &certificate_pem);

    /// Reads a whole (small) file into a string -- convenience for loading PEM material.
    [[nodiscard]] std::expected<std::string, IoError> read_text_file(const std::string &path);

} // namespace SFT::Async
