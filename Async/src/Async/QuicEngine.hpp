#pragma once

// Private: the QUIC connection/endpoint core on ngtcp2 (+ OpenSSL's QUIC TLS callbacks). It owns packet I/O, timers,
// address validation, connection-id routing and the write loop; what rides on top of the streams -- the public raw
// stream API (QuicImpl.cpp) or HTTP/3 (Http3Impl.cpp) -- plugs in as a `QuicApp`.
//
// Everything here runs on the endpoint's strand. Public entry points elsewhere must `asio::post(conn->strand(), ...)`.

#if defined(STURDY_ASYNC_QUIC)

#include <Async/Mutex.hpp>
#include <Async/NetInternal.hpp>
#include <Async/Quic.hpp>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_map>

namespace SFT::Async::Detail {

    using QuicStrand = asio::strand<asio::io_context::executor_type>;

    inline constexpr std::size_t kQuicServerCidLen = 18;

    class QuicConn;
    class QuicEndpoint;

    /// What sits on a connection's streams. All calls happen on the connection's strand.
    class QuicApp {
      public:
        virtual ~QuicApp() = default;

        virtual void handshake_completed() {}
        /// The peer confirmed the handshake (for a client: the server accepted our Finished, certificate included).
        virtual void handshake_confirmed() {}
        /// False: protocol violation -- the connection is closed.
        virtual bool recv_stream_data(std::int64_t stream_id, bool fin, const std::uint8_t *data, std::size_t size) = 0;
        virtual bool acked_stream_data(std::int64_t stream_id, std::uint64_t size) = 0;
        virtual void stream_open(std::int64_t) {}
        virtual void stream_close(std::int64_t stream_id, std::uint64_t app_error_code) = 0;
        virtual void stream_reset(std::int64_t, std::uint64_t) {}
        virtual void stream_stop_sending(std::int64_t, std::uint64_t) {}
        virtual void extend_max_local_streams(bool /*bidirectional*/) {}
        virtual void extend_max_stream_data(std::int64_t) {}
        virtual void recv_datagram(const std::uint8_t *, std::size_t) {}
        /// The connection ended (once). No further calls follow.
        virtual void closed(const QuicCloseInfo &info) = 0;

        // ---- the write loop's view of the app's pending output ----
        struct StreamWrite {
            std::int64_t stream_id = -1;
            bool fin = false;
            ngtcp2_vec vec[16]{};
            std::size_t veccnt = 0;
            std::uint64_t offered = 0; ///< bytes described by `vec`
        };
        /// Offers the next stream with something to send (round robin). False: nothing pending.
        virtual bool next_stream_write(StreamWrite &out) = 0;
        /// `bytes` of `out` were accepted into a packet (`fin`: the FIN too).
        virtual void stream_write_done(const StreamWrite &out, std::size_t bytes) = 0;
        /// Stream-level flow control is exhausted for this stream; stop offering it until `extend_max_stream_data`.
        virtual void stream_write_blocked(std::int64_t) {}
        /// The stream's sending side is gone (reset / STOP_SENDING).
        virtual void stream_write_shut(std::int64_t) {}
        /// Datagram output: fills the next datagram to send; false if none.
        virtual bool next_datagram(std::vector<std::uint8_t> &, std::uint64_t & /*id*/) { return false; }
        virtual void datagram_accepted(std::uint64_t /*id*/) {}
    };

    struct QuicEndpointConfig {
        bool server = false;
        QuicLimits limits;
        std::shared_ptr<asio::ssl::context> tls_context;
        // client
        TlsClientOptions client_tls;
        // server
        std::vector<std::string> pinned_client_sha256;
        std::size_t max_connections = 1024;
        std::size_t max_connections_per_address = 32;
        bool require_address_validation = true;
        /// Builds the app for a connection about to start (client: right after creation; server: on the first Initial).
        std::function<std::shared_ptr<QuicApp>(QuicConn &)> make_app;
        /// Server: a connection finished its handshake and may be handed to the application.
        std::function<void(const std::shared_ptr<QuicConn> &)> on_ready;
        /// Server: a connection ended before / after the handshake (for metrics and cleanup).
        std::function<void(const std::shared_ptr<QuicConn> &, bool handshake_was_complete)> on_ended;
    };

    class QuicConn : public std::enable_shared_from_this<QuicConn> {
      public:
        ~QuicConn();
        QuicConn(const QuicConn &) = delete;
        QuicConn &operator=(const QuicConn &) = delete;

        [[nodiscard]] QuicStrand &strand();
        [[nodiscard]] ngtcp2_conn *raw() const noexcept { return conn_; }
        [[nodiscard]] QuicApp *app() const noexcept { return app_.get(); }
        [[nodiscard]] bool is_server() const noexcept { return server_; }
        [[nodiscard]] bool closed() const noexcept { return closed_; }
        [[nodiscard]] bool handshake_complete() const noexcept { return handshake_done_; }
        [[nodiscard]] const QuicLimits &limits() const noexcept;
        [[nodiscard]] udp::endpoint remote() const { return remote_; }
        [[nodiscard]] TlsInfo tls_info() const;
        [[nodiscard]] QuicStats stats() const;
        [[nodiscard]] std::size_t max_datagram_size() const noexcept { return max_datagram_size_.load(std::memory_order_relaxed); }

        /// Run the write loop soon (coalesced). Thread-safe.
        void request_flush();
        /// Thread-safe: closes with an application error code and reports.
        void request_close(std::uint64_t code, std::string reason);

        // Strand-only from here.
        void flush();
        void close_now(std::uint64_t code, std::string reason);

        /// Closes the connection because the app hit a fatal error (protocol violation).
        void fail_protocol(std::uint64_t app_code, std::string reason);

      private:
        friend class QuicEndpoint;
        friend struct QuicCallbacks;

        QuicConn(std::shared_ptr<QuicEndpoint> endpoint, bool server);

        [[nodiscard]] Result<void> create_tls();
        [[nodiscard]] Result<void> setup_client(const udp::endpoint &remote, const std::string &host);
        [[nodiscard]] Result<void> setup_server(const udp::endpoint &remote, const ngtcp2_pkt_hd &hd, const ngtcp2_cid *original_dcid);
        void begin_closing(std::string reason_text);
        void schedule_removal();
        void on_packet(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size);
        void on_timer();
        void arm_timer();
        void report_closed(QuicCloseInfo info);
        void handle_read_error(int rv);
        void send_close_packet();
        void on_handshake_completed();
        void emit(const std::uint8_t *data, std::size_t size);
        void update_cached_state();

        std::shared_ptr<QuicEndpoint> endpoint_;
        bool server_;
        ngtcp2_conn *conn_ = nullptr;
        SSL *ssl_ = nullptr;
        ngtcp2_crypto_ossl_ctx *ossl_ = nullptr;
        ngtcp2_crypto_conn_ref conn_ref_{};
        std::shared_ptr<QuicApp> app_;
        asio::steady_timer timer_;
        std::uint64_t timer_generation_ = 0;
        std::atomic<bool> flush_queued_{false};
        udp::endpoint remote_;
        udp::endpoint local_;
        ngtcp2_path_storage path_{};
        bool handshake_done_ = false;
        bool closed_ = false;
        bool close_reported_ = false;
        bool destroying_ = false;
        bool pin_failed_ = false;
        std::vector<std::string> cids_;
        std::string host_;
        QuicCloseInfo close_info_;
        ngtcp2_ccerr last_error_{};
        std::string close_reason_; ///< storage for last_error_.reason
        std::atomic<std::size_t> max_datagram_size_{0};
        mutable Mutex<TlsInfo> tls_info_;
        mutable Mutex<QuicStats> stats_;
        std::string server_scid_; ///< first server-side CID, for routing the closing period
    };

    struct QuicEndpointMetrics {
        std::atomic<std::uint64_t> accepted{0};
        std::atomic<std::uint64_t> retries{0};
        std::atomic<std::uint64_t> rejected_limit{0};
        std::atomic<std::uint64_t> invalid_tokens{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> handshakes_failed{0};
        std::atomic<std::uint64_t> active{0};
    };

    /// One UDP socket and the connections that use it.
    class QuicEndpoint : public std::enable_shared_from_this<QuicEndpoint> {
      public:
        ~QuicEndpoint();

        [[nodiscard]] static Result<std::shared_ptr<QuicEndpoint>> create(QuicEndpointConfig config, const udp::endpoint &bind_to);

        /// Client: creates the connection to `remote` and starts the handshake. `host` is the name to verify.
        [[nodiscard]] Result<std::shared_ptr<QuicConn>> connect(const udp::endpoint &remote, const std::string &host);

        void start();
        void stop();

        [[nodiscard]] QuicStrand &strand() { return strand_; }
        [[nodiscard]] const QuicEndpointConfig &config() const { return config_; }
        [[nodiscard]] std::uint16_t local_port() const { return local_.port(); }
        [[nodiscard]] udp::endpoint local() const { return local_; }
        [[nodiscard]] QuicEndpointMetrics &metrics() { return metrics_; }
        [[nodiscard]] const std::array<std::uint8_t, 32> &secret() const { return secret_; }

      private:
        friend class QuicConn;
        friend struct QuicCallbacks;

        explicit QuicEndpoint(QuicEndpointConfig config);

        void receive_next();
        void on_datagram(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size);
        void handle_initial(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size, const ngtcp2_version_cid &vc);
        void send_to(const udp::endpoint &to, const std::uint8_t *data, std::size_t size);
        void send_version_negotiation(const udp::endpoint &to, const ngtcp2_version_cid &vc);
        void send_stateless_reset(const udp::endpoint &to, const ngtcp2_version_cid &vc, std::size_t received_size);
        void refuse(const udp::endpoint &to, const ngtcp2_pkt_hd &hd, std::uint64_t transport_error);
        void register_cid(const std::string &cid, const std::shared_ptr<QuicConn> &conn);
        void unregister_cid(const std::string &cid);
        void connection_ended(const std::shared_ptr<QuicConn> &conn);
        [[nodiscard]] std::string address_key(const udp::endpoint &endpoint) const;

        QuicStrand strand_;
        udp::socket socket_;
        QuicEndpointConfig config_;
        udp::endpoint local_;
        std::array<std::uint8_t, 32> secret_{};
        std::array<std::uint8_t, 64 * 1024> receive_buffer_{};
        udp::endpoint sender_;
        bool stopped_ = false;
        bool receiving_ = false;
        QuicEndpointMetrics metrics_;

        // server routing (strand only)
        std::unordered_map<std::string, std::shared_ptr<QuicConn>> by_cid_;
        std::map<std::string, std::size_t> per_address_;
        std::size_t live_connections_ = 0;
        std::shared_ptr<QuicConn> client_conn_;
        std::uint64_t reset_tokens_ = 16; ///< stateless-reset rate limit (token bucket)
        std::chrono::steady_clock::time_point reset_refill_ = std::chrono::steady_clock::now();
    };

    [[nodiscard]] ngtcp2_tstamp quic_now();

    /// TLS 1.3 only, no tickets or 0-RTT (defined in QuicImpl.cpp).
    void harden_quic_context(SSL_CTX *context, bool server);
    /// Numeric host (IPv4-mapped IPv6 addresses shown as IPv4) and port.
    [[nodiscard]] SocketAddress to_socket_address(const udp::endpoint &endpoint);

} // namespace SFT::Async::Detail

#endif
