#pragma once

// Private to Async's .cpp files: Boost.Asio is a PRIVATE dependency, so nothing here may be included
// from a public header.

#include <Async/IoError.hpp>
#include <Async/Promise.hpp>
#include <Async/Tls.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace SFT::Async::Detail {

    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    using udp = asio::ip::udp;
    using ErrorCode = boost::system::error_code;

    /// The process-wide network reactor: an `io_context` driven by a small thread pool. Every socket in
    /// the engine's Async layer lives on it, so thousands of idle connections cost no worker threads.
    /// Completion of a `Promise` happens on a reactor thread -- continuations should hop with `then<Rt>`.
    [[nodiscard]] asio::io_context &reactor();

    /// Number of reactor threads (fixed at first use; `configure_reactor` must precede it).
    [[nodiscard]] bool configure_reactor(unsigned thread_count);
    [[nodiscard]] unsigned reactor_thread_count();
    [[nodiscard]] bool on_reactor_thread() noexcept;

    [[nodiscard]] IoError to_io_error(const char *operation, const ErrorCode &error);

    template <typename T>
    using Result = std::expected<T, IoError>;

    /// Completes `promise` with a value / an error without caring which type it carries.
    template <typename T>
    void finish(Promise<Result<T>> &promise, Result<T> result) {
        (void)promise.set_value(std::move(result));
    }

    template <typename T>
    void finish_cancelled(Promise<Result<T>> &promise) {
        (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
    }

    [[nodiscard]] Result<std::shared_ptr<asio::ssl::context>> make_tls_client_context(const TlsClientOptions &options);
    [[nodiscard]] Result<std::shared_ptr<asio::ssl::context>> make_tls_server_context(const TlsServerOptions &options);

    /// Applies SNI / host-name verification / pinning on a client stream before its handshake.
    [[nodiscard]] Result<void> prepare_tls_client(SSL *ssl, const TlsClientOptions &options, const std::string &connect_host);

    /// Post-handshake exact-certificate check. Empty `pins` always passes.
    [[nodiscard]] Result<void> check_certificate_pin(SSL *ssl, const std::vector<std::string> &pins);

    [[nodiscard]] TlsInfo describe_tls(SSL *ssl);

    [[nodiscard]] std::string to_hex(const unsigned char *data, std::size_t size);
    [[nodiscard]] std::string lowercase(std::string text);

} // namespace SFT::Async::Detail
