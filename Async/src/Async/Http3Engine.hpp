#pragma once

// Private: HTTP/3 (RFC 9114) over the QUIC core, on nghttp3 -- usable as server and client. It is a `QuicApp`: the
// connection's streams belong to nghttp3, which does framing, QPACK header compression and the control streams; this
// layer adds limits, streaming bodies and the mapping to Async's HTTP types (the same ones the HTTP/2 engine uses).
//
// Everything runs on the QUIC connection's strand; the public entry points post onto it.

#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)

#include <Async/HttpInternal.hpp>
#include <Async/QuicEngine.hpp>

#include <nghttp3/nghttp3.h>

#include <deque>
#include <functional>
#include <unordered_map>

namespace SFT::Async::Detail {

    struct H3ServerConfig {
        std::uint64_t max_request_body_bytes = 1 << 20;
        std::uint32_t max_header_bytes = 32 * 1024;
        std::uint64_t max_response_body_bytes = 8 << 20;
        std::string remote_address;
        /// Hands a request to the application; `respond` must be called exactly once, from any thread.
        std::function<void(HttpRequestMessage, std::function<void(HttpResponseMessage)> respond)> dispatch;
        std::function<void(unsigned status)> on_response; ///< metrics hook
    };

    struct H3ClientConfig {
        std::uint64_t max_response_body_bytes = 16 << 20;
        std::uint32_t max_header_bytes = 64 * 1024;
    };

    struct H3Stream;

    class H3Session final : public QuicApp {
      public:
        H3Session(QuicConn &conn, bool server, H3ServerConfig server_config, H3ClientConfig client_config);
        ~H3Session() override;

        /// Client: fires once when the connection is ready for requests, or with the reason it never was.
        std::function<void(Result<void>)> on_ready;

        /// Client: sends a request on a new stream. The handle fails with `ConnectionReset` if the connection can no
        /// longer take streams (GOAWAY / closed) -- the caller may retry on a fresh connection. Thread-safe.
        [[nodiscard]] TaskHandle<std::expected<HttpResponseMessage, IoError>> submit(HttpRequestMessage request, std::string scheme, std::string authority,
                                                                                      CancellationToken cancellation);
        /// Client: false once the server sent GOAWAY or the connection died.
        [[nodiscard]] bool usable() const noexcept { return usable_.load(std::memory_order_acquire); }

        // ---- QuicApp ----
        void handshake_completed() override;
        bool recv_stream_data(std::int64_t stream_id, bool fin, const std::uint8_t *data, std::size_t size) override;
        bool acked_stream_data(std::int64_t stream_id, std::uint64_t size) override;
        void stream_close(std::int64_t stream_id, std::uint64_t app_error_code) override;
        void stream_reset(std::int64_t stream_id, std::uint64_t app_error_code) override;
        void stream_stop_sending(std::int64_t stream_id, std::uint64_t app_error_code) override;
        void extend_max_local_streams(bool bidirectional) override;
        void extend_max_stream_data(std::int64_t stream_id) override;
        void closed(const QuicCloseInfo &info) override;
        bool next_stream_write(StreamWrite &out) override;
        void stream_write_done(const StreamWrite &out, std::size_t bytes) override;
        void stream_write_blocked(std::int64_t stream_id) override;
        void stream_write_shut(std::int64_t stream_id) override;

      private:
        friend struct H3Callbacks;

        void request_complete(const std::shared_ptr<H3Stream> &stream);
        void submit_response(std::int64_t id, HttpResponseMessage response);
        void start_fetch(const std::shared_ptr<H3Stream> &stream);
        nghttp3_ssize read_body(H3Stream &stream, nghttp3_vec *vec, std::size_t veccnt, std::uint32_t *flags);
        void finish_stream(const std::shared_ptr<H3Stream> &stream);
        void open_pending_requests();
        bool start_request(const std::shared_ptr<H3Stream> &stream, HttpRequestMessage &request, const std::string &scheme, const std::string &authority);

        QuicConn *conn_;
        bool server_;
        H3ServerConfig server_config_;
        H3ClientConfig client_config_;
        nghttp3_conn *h3_ = nullptr;
        std::unordered_map<std::int64_t, std::shared_ptr<H3Stream>> streams_;
        struct QueuedRequest {
            std::shared_ptr<H3Stream> stream;
            HttpRequestMessage request;
            std::string scheme;
            std::string authority;
        };
        std::deque<QueuedRequest> queued_; ///< client requests waiting for the server's stream limit
        std::atomic<bool> usable_{true};
        bool closed_ = false;
    };

    /// Starts an HTTP/3 server on `bind_to` (UDP). `tls` needs a certificate; ALPN is set to "h3" here.
    [[nodiscard]] Result<std::shared_ptr<QuicEndpoint>> start_h3_server(const udp::endpoint &bind_to, TlsServerOptions tls, H3ServerConfig config,
                                                                         std::uint32_t max_concurrent_streams, std::size_t max_connections,
                                                                         std::size_t max_connections_per_address);

    /// A client connection speaking HTTP/3. Keeps the QUIC connection (and with it the socket and the session) alive.
    struct H3Client {
        std::shared_ptr<QuicConn> conn;
        H3Session *session = nullptr;
        [[nodiscard]] bool usable() const noexcept { return session != nullptr && session->usable(); }
        void close() {
            if (conn) conn->request_close(0, "client closing");
        }
        ~H3Client() { close(); }
    };

    /// Connects to `address` (verifying `host` as in `TlsClientOptions`), completes the QUIC + HTTP/3 setup.
    [[nodiscard]] TaskHandle<Result<std::shared_ptr<H3Client>>> connect_h3(udp::endpoint address, std::string host, TlsClientOptions tls, H3ClientConfig config,
                                                                           std::chrono::milliseconds handshake_timeout);

} // namespace SFT::Async::Detail

#endif
