#include <Async/QuicEngine.hpp>

#if defined(STURDY_ASYNC_QUIC) && !defined(STURDY_PLATFORM_WEB)

#include <openssl/rand.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstring>

namespace SFT::Async::Detail {

    ngtcp2_tstamp quic_now() {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    namespace {

        std::string cid_key(const ngtcp2_cid &cid) { return std::string(reinterpret_cast<const char *>(cid.data), cid.datalen); }
        std::string cid_key(const std::uint8_t *data, std::size_t size) { return std::string(reinterpret_cast<const char *>(data), size); }

        udp::endpoint to_endpoint(const ngtcp2_sockaddr *sa) {
            if (sa->sa_family == AF_INET) {
                const auto *in = reinterpret_cast<const sockaddr_in *>(sa);
                asio::ip::address_v4::bytes_type bytes;
                std::memcpy(bytes.data(), &in->sin_addr, 4);
                return udp::endpoint(asio::ip::address_v4(bytes), ntohs(in->sin_port));
            }
            const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
            asio::ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &in6->sin6_addr, 16);
            return udp::endpoint(asio::ip::address_v6(bytes, in6->sin6_scope_id), ntohs(in6->sin6_port));
        }

        ngtcp2_cid random_cid(std::size_t length) {
            ngtcp2_cid cid;
            cid.datalen = length;
            RAND_bytes(cid.data, static_cast<int>(length));
            return cid;
        }

        void fill_params(ngtcp2_transport_params &params, const QuicLimits &limits) {
            ngtcp2_transport_params_default(&params);
            params.initial_max_stream_data_bidi_local = limits.stream_receive_window;
            params.initial_max_stream_data_bidi_remote = limits.stream_receive_window;
            params.initial_max_stream_data_uni = limits.stream_receive_window;
            params.initial_max_data = limits.connection_receive_window;
            params.initial_max_streams_bidi = limits.max_bidirectional_streams;
            params.initial_max_streams_uni = limits.max_unidirectional_streams;
            params.max_idle_timeout = static_cast<ngtcp2_duration>(limits.idle_timeout.count()) * NGTCP2_MILLISECONDS;
            params.active_connection_id_limit = 7;
            params.max_datagram_frame_size = limits.datagrams ? 65535 : 0;
        }

        void fill_settings(ngtcp2_settings &settings, const QuicLimits &limits) {
            ngtcp2_settings_default(&settings);
            settings.initial_ts = quic_now();
            settings.handshake_timeout = static_cast<ngtcp2_duration>(limits.handshake_timeout.count()) * NGTCP2_MILLISECONDS;
        }

        std::once_flag crypto_once;
        bool crypto_ready = false;

    } // namespace

    // =================================================================================================
    // ngtcp2 callbacks
    // =================================================================================================
    struct QuicCallbacks {
        static QuicConn *self(void *user_data) { return static_cast<QuicConn *>(user_data); }

        template <typename Fn>
        static int guarded(void *user_data, Fn &&fn) {
            try {
                return fn(*self(user_data));
            } catch (...) {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
        }

        static int handshake_completed(ngtcp2_conn *, void *user_data) {
            return guarded(user_data, [](QuicConn &c) {
                c.on_handshake_completed();
                return c.pin_failed_ ? NGTCP2_ERR_CALLBACK_FAILURE : 0;
            });
        }
        static int handshake_confirmed(ngtcp2_conn *, void *user_data) {
            return guarded(user_data, [](QuicConn &c) {
                c.app_->handshake_confirmed();
                return 0;
            });
        }
        static int recv_stream_data(ngtcp2_conn *, std::uint32_t flags, std::int64_t id, std::uint64_t, const std::uint8_t *data, std::size_t size, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) {
                return c.app_->recv_stream_data(id, (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0, data, size) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
            });
        }
        static int acked_stream_data_offset(ngtcp2_conn *, std::int64_t id, std::uint64_t, std::uint64_t size, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) { return c.app_->acked_stream_data(id, size) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE; });
        }
        static int stream_open(ngtcp2_conn *, std::int64_t id, void *user_data) {
            return guarded(user_data, [&](QuicConn &c) {
                c.app_->stream_open(id);
                return 0;
            });
        }
        static int stream_close(ngtcp2_conn *conn, std::uint32_t flags, std::int64_t id, std::uint64_t app_error_code, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) {
                if ((flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) == 0) app_error_code = 0;
                c.app_->stream_close(id, app_error_code);
                // The peer's streams count against its limit until we say they are done.
                if (ngtcp2_conn_is_local_stream(conn, id) == 0) {
                    if (ngtcp2_is_bidi_stream(id)) ngtcp2_conn_extend_max_streams_bidi(conn, 1);
                    else ngtcp2_conn_extend_max_streams_uni(conn, 1);
                }
                return 0;
            });
        }
        static int stream_reset(ngtcp2_conn *, std::int64_t id, std::uint64_t, std::uint64_t app_error_code, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) {
                c.app_->stream_reset(id, app_error_code);
                return 0;
            });
        }
        static int stream_stop_sending(ngtcp2_conn *, std::int64_t id, std::uint64_t app_error_code, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) {
                c.app_->stream_stop_sending(id, app_error_code);
                return 0;
            });
        }
        static int extend_max_local_streams_bidi(ngtcp2_conn *, std::uint64_t, void *user_data) {
            return guarded(user_data, [](QuicConn &c) {
                c.app_->extend_max_local_streams(true);
                return 0;
            });
        }
        static int extend_max_local_streams_uni(ngtcp2_conn *, std::uint64_t, void *user_data) {
            return guarded(user_data, [](QuicConn &c) {
                c.app_->extend_max_local_streams(false);
                return 0;
            });
        }
        static int extend_max_stream_data(ngtcp2_conn *, std::int64_t id, std::uint64_t, void *user_data, void *) {
            return guarded(user_data, [&](QuicConn &c) {
                c.app_->extend_max_stream_data(id);
                return 0;
            });
        }
        static int recv_datagram(ngtcp2_conn *, std::uint32_t, const std::uint8_t *data, std::size_t size, void *user_data) {
            return guarded(user_data, [&](QuicConn &c) {
                c.app_->recv_datagram(data, size);
                return 0;
            });
        }
        static void rand(std::uint8_t *dest, std::size_t size, const ngtcp2_rand_ctx *) { RAND_bytes(dest, static_cast<int>(size)); }

        static int get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token, std::size_t cid_length, void *user_data) {
            return guarded(user_data, [&](QuicConn &c) {
                if (RAND_bytes(cid->data, static_cast<int>(cid_length)) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
                cid->datalen = cid_length;
                const auto &secret = c.endpoint_->secret();
                if (ngtcp2_crypto_generate_stateless_reset_token(token->data, secret.data(), secret.size(), cid) != 0) return NGTCP2_ERR_CALLBACK_FAILURE;
                if (c.server_) {
                    const std::string key = cid_key(*cid);
                    c.endpoint_->register_cid(key, c.shared_from_this());
                    c.cids_.push_back(key);
                }
                return 0;
            });
        }
        static int remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) {
            return guarded(user_data, [&](QuicConn &c) {
                if (c.server_ && !c.destroying_) c.endpoint_->unregister_cid(cid_key(*cid));
                return 0;
            });
        }
        static int path_validation(ngtcp2_conn *, std::uint32_t, const ngtcp2_path *, const ngtcp2_path *, ngtcp2_path_validation_result, void *) { return 0; }
    };

    static ngtcp2_callbacks make_callbacks(bool server) {
        ngtcp2_callbacks cb{};
        if (server) cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
        else cb.client_initial = ngtcp2_crypto_client_initial_cb;
        cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        cb.handshake_completed = QuicCallbacks::handshake_completed;
        cb.handshake_confirmed = QuicCallbacks::handshake_confirmed;
        cb.encrypt = ngtcp2_crypto_encrypt_cb;
        cb.decrypt = ngtcp2_crypto_decrypt_cb;
        cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
        cb.recv_stream_data = QuicCallbacks::recv_stream_data;
        cb.acked_stream_data_offset = QuicCallbacks::acked_stream_data_offset;
        cb.stream_open = QuicCallbacks::stream_open;
        cb.stream_close = QuicCallbacks::stream_close;
        cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
        cb.extend_max_local_streams_bidi = QuicCallbacks::extend_max_local_streams_bidi;
        cb.extend_max_local_streams_uni = QuicCallbacks::extend_max_local_streams_uni;
        cb.rand = QuicCallbacks::rand;
        cb.get_new_connection_id2 = QuicCallbacks::get_new_connection_id;
        cb.remove_connection_id = QuicCallbacks::remove_connection_id;
        cb.update_key = ngtcp2_crypto_update_key_cb;
        cb.path_validation = QuicCallbacks::path_validation;
        cb.stream_reset = QuicCallbacks::stream_reset;
        cb.extend_max_stream_data = QuicCallbacks::extend_max_stream_data;
        cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        cb.recv_datagram = QuicCallbacks::recv_datagram;
        cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        cb.stream_stop_sending = QuicCallbacks::stream_stop_sending;
        cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        return cb;
    }

    // =================================================================================================
    // QuicConn
    // =================================================================================================
    QuicConn::QuicConn(std::shared_ptr<QuicEndpoint> endpoint, bool server)
        : endpoint_(std::move(endpoint)), server_(server), timer_(endpoint_->strand_) {
        ngtcp2_ccerr_default(&last_error_);
    }

    QuicConn::~QuicConn() {
        destroying_ = true;
        if (conn_ != nullptr) ngtcp2_conn_del(conn_);
        if (ossl_ != nullptr) ngtcp2_crypto_ossl_ctx_del(ossl_);
        if (ssl_ != nullptr) SSL_free(ssl_);
    }

    QuicStrand &QuicConn::strand() { return endpoint_->strand_; }
    const QuicLimits &QuicConn::limits() const noexcept { return endpoint_->config_.limits; }

    TlsInfo QuicConn::tls_info() const {
        auto guard = tls_info_.lock();
        return *guard;
    }
    QuicStats QuicConn::stats() const {
        auto guard = stats_.lock();
        return *guard;
    }

    Result<void> QuicConn::create_tls() {
        ssl_ = SSL_new(endpoint_->config_.tls_context->native_handle());
        if (ssl_ == nullptr) return std::unexpected(IoError{IoErrorCode::TlsFailure, "cannot create a TLS session"});
        if (ngtcp2_crypto_ossl_ctx_new(&ossl_, ssl_) != 0) return std::unexpected(IoError{IoErrorCode::TlsFailure, "cannot create the QUIC TLS context"});
        const int configured = server_ ? ngtcp2_crypto_ossl_configure_server_session(ssl_) : ngtcp2_crypto_ossl_configure_client_session(ssl_);
        if (configured != 0) return std::unexpected(IoError{IoErrorCode::TlsFailure, "OpenSSL on this system does not support QUIC TLS (needs 3.5+)"});
        conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref *ref) { return static_cast<QuicConn *>(ref->user_data)->conn_; };
        conn_ref_.user_data = this;
        SSL_set_app_data(ssl_, &conn_ref_);
        if (server_) SSL_set_accept_state(ssl_);
        else SSL_set_connect_state(ssl_);
        return {};
    }

    Result<void> QuicConn::setup_client(const udp::endpoint &remote, const std::string &host) {
        host_ = host;
        remote_ = remote;
        local_ = endpoint_->local_;
        if (auto tls = create_tls(); !tls) return tls;
        if (auto prepared = prepare_tls_client(ssl_, endpoint_->config_.client_tls, host); !prepared) return prepared;

        ngtcp2_path_storage_init(&path_, local_.data(), local_.size(), remote_.data(), remote_.size(), nullptr);
        const ngtcp2_cid dcid = random_cid(kQuicServerCidLen);
        const ngtcp2_cid scid = random_cid(kQuicServerCidLen);
        ngtcp2_settings settings;
        fill_settings(settings, limits());
        ngtcp2_transport_params params;
        fill_params(params, limits());
        const ngtcp2_callbacks callbacks = make_callbacks(false);
        const int rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path_.path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, this);
        if (rv != 0) return std::unexpected(IoError{IoErrorCode::Unknown, std::string("ngtcp2_conn_client_new: ") + ngtcp2_strerror(rv)});
        ngtcp2_conn_set_tls_native_handle(conn_, ossl_);
        return {};
    }

    Result<void> QuicConn::setup_server(const udp::endpoint &remote, const ngtcp2_pkt_hd &hd, const ngtcp2_cid *original_dcid) {
        remote_ = remote;
        local_ = endpoint_->local_;
        if (auto tls = create_tls(); !tls) return tls;

        ngtcp2_path_storage_init(&path_, local_.data(), local_.size(), remote_.data(), remote_.size(), nullptr);
        const ngtcp2_cid scid = random_cid(kQuicServerCidLen);
        ngtcp2_settings settings;
        fill_settings(settings, limits());
        if (original_dcid != nullptr) {
            settings.token = hd.token;
            settings.tokenlen = hd.tokenlen;
            settings.token_type = NGTCP2_TOKEN_TYPE_RETRY;
        }
        ngtcp2_transport_params params;
        fill_params(params, limits());
        params.original_dcid = original_dcid != nullptr ? *original_dcid : hd.dcid;
        params.original_dcid_present = 1;
        if (original_dcid != nullptr) {
            params.retry_scid = hd.dcid;
            params.retry_scid_present = 1;
        }
        const auto &secret = endpoint_->secret();
        if (ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token, secret.data(), secret.size(), &scid) != 0) {
            return std::unexpected(IoError{IoErrorCode::Unknown, "cannot derive the stateless reset token"});
        }
        params.stateless_reset_token_present = 1;

        const ngtcp2_callbacks callbacks = make_callbacks(true);
        const int rv = ngtcp2_conn_server_new(&conn_, &hd.scid, &scid, &path_.path, hd.version, &callbacks, &settings, &params, nullptr, this);
        if (rv != 0) return std::unexpected(IoError{IoErrorCode::Unknown, std::string("ngtcp2_conn_server_new: ") + ngtcp2_strerror(rv)});
        ngtcp2_conn_set_tls_native_handle(conn_, ossl_);
        // Route by both the id we chose and the one the client first used (its Initial may be retransmitted).
        for (const std::string &key : {cid_key(scid), cid_key(hd.dcid)}) {
            endpoint_->register_cid(key, shared_from_this());
            cids_.push_back(key);
        }
        return {};
    }

    void QuicConn::request_flush() {
        if (flush_queued_.exchange(true)) return;
        asio::post(strand(), [self = shared_from_this()] {
            self->flush_queued_ = false;
            self->flush();
        });
    }

    void QuicConn::request_close(std::uint64_t code, std::string reason) {
        asio::post(strand(), [self = shared_from_this(), code, reason = std::move(reason)]() mutable { self->close_now(code, std::move(reason)); });
    }

    void QuicConn::emit(const std::uint8_t *data, std::size_t size) {
        endpoint_->send_to(to_endpoint(path_.path.remote.addr), data, size);
    }

    void QuicConn::update_cached_state() {
        if (conn_ == nullptr || closed_) return;
        ngtcp2_conn_info info{};
        ngtcp2_conn_get_conn_info(conn_, &info);
        {
            auto guard = stats_.lock();
            guard->smoothed_rtt = std::chrono::microseconds(info.smoothed_rtt / 1000);
            guard->min_rtt = std::chrono::microseconds(info.min_rtt / 1000);
            guard->bytes_in_flight = info.bytes_in_flight;
            guard->congestion_window = info.cwnd;
            guard->packets_lost = info.pkt_lost;
            guard->bytes_sent = info.bytes_sent;
            guard->bytes_received = info.bytes_recv;
        }
        std::size_t datagram = 0;
        if (handshake_done_ && limits().datagrams) {
            if (const auto *remote_params = ngtcp2_conn_get_remote_transport_params(conn_)) {
                const std::size_t path = ngtcp2_conn_get_path_max_tx_udp_payload_size(conn_);
                constexpr std::size_t overhead = 1 + kQuicServerCidLen + 4 + 3 + 16; // short header + datagram frame + AEAD tag
                const std::size_t by_path = path > overhead ? path - overhead : 0;
                const std::size_t by_peer = remote_params->max_datagram_frame_size > 3 ? static_cast<std::size_t>(remote_params->max_datagram_frame_size) - 3 : 0;
                datagram = (std::min)(by_path, by_peer);
            }
        }
        max_datagram_size_.store(datagram, std::memory_order_relaxed);
    }

    void QuicConn::flush() {
        if (closed_ || conn_ == nullptr) return;
        if (ngtcp2_conn_in_closing_period(conn_) != 0 || ngtcp2_conn_in_draining_period(conn_) != 0) return;
        std::array<std::uint8_t, 2048> buffer;
        ngtcp2_pkt_info pi{};
        const std::size_t buffer_size = (std::min)(buffer.size(), ngtcp2_conn_get_max_tx_udp_payload_size(conn_));
        constexpr int kMaxPacketsPerRound = 64;
        int packets = 0;
        std::vector<std::uint8_t> datagram;

        for (; packets < kMaxPacketsPerRound; ++packets) {
            const ngtcp2_tstamp ts = quic_now();
            ngtcp2_ssize nwrite = 0;

            // Unreliable datagrams go first: they are small and usually time-sensitive.
            std::uint64_t datagram_id = 0;
            if (handshake_done_ && app_->next_datagram(datagram, datagram_id)) {
                ngtcp2_vec vec{datagram.data(), datagram.size()};
                int accepted = 0;
                nwrite = ngtcp2_conn_writev_datagram(conn_, &path_.path, &pi, buffer.data(), buffer_size, &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_NONE,
                                                     datagram_id, &vec, 1, ts);
                if (accepted != 0) app_->datagram_accepted(datagram_id);
                if (nwrite < 0) {
                    ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
                    begin_closing("failed to write a datagram");
                    return;
                }
                if (nwrite > 0) {
                    emit(buffer.data(), static_cast<std::size_t>(nwrite));
                    continue;
                }
            }

            QuicApp::StreamWrite write;
            // Streams carry data once the handshake produced 1-RTT keys, and only while connection-level credit remains
            // (when it runs out nothing is offered; the next MAX_DATA frame re-runs this loop).
            const bool have = handshake_done_ && ngtcp2_conn_get_max_data_left(conn_) > 0 && app_->next_stream_write(write);
            std::uint32_t flags = 0;
            if (have && write.fin) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            ngtcp2_ssize ndatalen = -1;
            nwrite = ngtcp2_conn_writev_stream(conn_, &path_.path, &pi, buffer.data(), buffer_size, &ndatalen, flags, have ? write.stream_id : -1,
                                               have && write.veccnt > 0 ? write.vec : nullptr, have ? write.veccnt : 0, ts);
            if (nwrite < 0) {
                switch (nwrite) {
                    case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                        if (have) app_->stream_write_blocked(write.stream_id);
                        continue;
                    case NGTCP2_ERR_STREAM_SHUT_WR:
                        if (have) app_->stream_write_shut(write.stream_id);
                        continue;
                    case NGTCP2_ERR_STREAM_NOT_FOUND:
                        if (have) app_->stream_write_shut(write.stream_id);
                        continue;
                    case NGTCP2_ERR_WRITE_MORE:
                        if (have && ndatalen >= 0) app_->stream_write_done(write, static_cast<std::size_t>(ndatalen));
                        continue;
                    default:
                        ngtcp2_ccerr_set_liberr(&last_error_, static_cast<int>(nwrite), nullptr, 0);
                        begin_closing(std::string("QUIC write failed: ") + ngtcp2_strerror(static_cast<int>(nwrite)));
                        return;
                }
            }
            if (have && ndatalen >= 0) app_->stream_write_done(write, static_cast<std::size_t>(ndatalen));
            if (nwrite == 0) break; // congestion- or pacing-limited, or nothing to say
            emit(buffer.data(), static_cast<std::size_t>(nwrite));
        }
        if (packets >= kMaxPacketsPerRound) request_flush(); // yield to the reactor, then continue
        update_cached_state();
        arm_timer();
    }

    void QuicConn::arm_timer() {
        if (closed_ || conn_ == nullptr) return;
        const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
        if (expiry == UINT64_MAX) {
            timer_.cancel();
            ++timer_generation_;
            return;
        }
        timer_.expires_at(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(expiry)));
        const std::uint64_t generation = ++timer_generation_;
        timer_.async_wait(asio::bind_executor(strand(), [self = shared_from_this(), generation](const ErrorCode &ec) {
            if (ec || generation != self->timer_generation_) return;
            self->on_timer();
        }));
    }

    void QuicConn::on_timer() {
        if (closed_) return;
        const int rv = ngtcp2_conn_handle_expiry(conn_, quic_now());
        if (rv != 0) {
            if (rv == NGTCP2_ERR_IDLE_CLOSE || rv == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
                QuicCloseInfo info;
                info.application = false;
                info.timed_out = true;
                info.reason = rv == NGTCP2_ERR_IDLE_CLOSE ? "connection idle timeout" : "handshake timed out";
                closed_ = true;
                if (!handshake_done_) ++endpoint_->metrics_.handshakes_failed;
                report_closed(std::move(info));
                schedule_removal();
                return;
            }
            ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
            begin_closing(std::string("QUIC timer error: ") + ngtcp2_strerror(rv));
            return;
        }
        flush();
    }

    void QuicConn::on_packet(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size) {
        if (closed_ || conn_ == nullptr) return;
        ngtcp2_path path;
        path.local = {const_cast<ngtcp2_sockaddr *>(local_.data()), static_cast<ngtcp2_socklen>(local_.size())};
        path.remote = {const_cast<ngtcp2_sockaddr *>(remote.data()), static_cast<ngtcp2_socklen>(remote.size())};
        path.user_data = nullptr;
        ngtcp2_pkt_info pi{};
        const int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data, size, quic_now());
        if (rv != 0) {
            handle_read_error(rv);
            return;
        }
        flush();
    }

    void QuicConn::handle_read_error(int rv) {
        if (closed_) return;
        switch (rv) {
            case NGTCP2_ERR_DRAINING: {
                const ngtcp2_ccerr *error = ngtcp2_conn_get_ccerr(conn_);
                QuicCloseInfo info;
                info.from_peer = true;
                if (error != nullptr) {
                    info.code = error->error_code;
                    info.application = error->type == NGTCP2_CCERR_TYPE_APPLICATION;
                    if (error->reason != nullptr && error->reasonlen > 0) info.reason.assign(reinterpret_cast<const char *>(error->reason), error->reasonlen);
                }
                closed_ = true;
                report_closed(std::move(info));
                schedule_removal();
                return;
            }
            case NGTCP2_ERR_DROP_CONN: {
                QuicCloseInfo info;
                info.application = false;
                info.reason = "connection dropped";
                closed_ = true;
                if (!handshake_done_) ++endpoint_->metrics_.handshakes_failed;
                report_closed(std::move(info));
                schedule_removal();
                return;
            }
            case NGTCP2_ERR_CRYPTO: {
                if (last_error_.error_code == 0) ngtcp2_ccerr_set_tls_alert(&last_error_, ngtcp2_conn_get_tls_alert(conn_), nullptr, 0);
                std::string why = "TLS handshake failed";
                const long verify = SSL_get_verify_result(ssl_);
                if (verify != X509_V_OK) why += std::string(": ") + X509_verify_cert_error_string(verify);
                begin_closing(why);
                return;
            }
            default:
                if (!pin_failed_) ngtcp2_ccerr_set_liberr(&last_error_, rv, nullptr, 0);
                begin_closing(pin_failed_ ? "certificate does not match the pinned identity" : std::string("QUIC error: ") + ngtcp2_strerror(rv));
                return;
        }
    }

    /// We failed the connection: say so on the wire once, then report and wait out the closing period.
    void QuicConn::begin_closing(std::string reason_text) {
        if (closed_) return;
        if (!handshake_done_) ++endpoint_->metrics_.handshakes_failed;
        send_close_packet();
        QuicCloseInfo info;
        info.code = last_error_.error_code;
        info.application = false;
        info.reason = std::move(reason_text);
        closed_ = true;
        report_closed(std::move(info));
        schedule_removal();
    }

    void QuicConn::send_close_packet() {
        std::array<std::uint8_t, 2048> buffer;
        ngtcp2_pkt_info pi{};
        const std::size_t size = (std::min)(buffer.size(), ngtcp2_conn_get_max_tx_udp_payload_size(conn_));
        const ngtcp2_ssize n = ngtcp2_conn_write_connection_close(conn_, &path_.path, &pi, buffer.data(), size, &last_error_, quic_now());
        if (n > 0) emit(buffer.data(), static_cast<std::size_t>(n));
    }

    void QuicConn::close_now(std::uint64_t code, std::string reason) {
        if (closed_ || conn_ == nullptr) return;
        close_reason_ = std::move(reason);
        ngtcp2_ccerr_set_application_error(&last_error_, code, reinterpret_cast<const std::uint8_t *>(close_reason_.data()), close_reason_.size());
        send_close_packet();
        QuicCloseInfo info;
        info.code = code;
        info.reason = close_reason_;
        info.application = true;
        closed_ = true;
        report_closed(std::move(info));
        schedule_removal();
    }

    void QuicConn::fail_protocol(std::uint64_t app_code, std::string reason) { close_now(app_code, std::move(reason)); }

    void QuicConn::report_closed(QuicCloseInfo info) {
        if (close_reported_) return;
        close_reported_ = true;
        close_info_ = info;
        timer_.cancel();
        ++timer_generation_;
        if (app_) app_->closed(info);
    }

    /// Keeps the connection id mapped for a few round trips so late packets are absorbed, then lets go.
    void QuicConn::schedule_removal() {
        ngtcp2_duration pto = 200 * NGTCP2_MILLISECONDS;
        if (conn_ != nullptr) pto = (std::max)(pto, ngtcp2_conn_get_pto(conn_));
        timer_.expires_after(std::chrono::nanoseconds(pto * 3));
        const std::uint64_t generation = ++timer_generation_;
        timer_.async_wait(asio::bind_executor(strand(), [self = shared_from_this(), generation](const ErrorCode &ec) {
            if (ec || generation != self->timer_generation_) return;
            self->endpoint_->connection_ended(self);
        }));
    }

    void QuicConn::on_handshake_completed() {
        handshake_done_ = true;
        const auto &config = endpoint_->config_;
        const auto &pins = server_ ? config.pinned_client_sha256 : config.client_tls.pinned_sha256;
        if (auto pinned = check_certificate_pin(ssl_, pins); !pinned) {
            pin_failed_ = true;
            ngtcp2_ccerr_set_tls_alert(&last_error_, 42 /* bad_certificate */, nullptr, 0);
            return;
        }
        {
            auto guard = tls_info_.lock();
            *guard = describe_tls(ssl_);
        }
        if (limits().keep_alive.count() > 0) {
            ngtcp2_conn_set_keep_alive_timeout(conn_, static_cast<ngtcp2_duration>(limits().keep_alive.count()) * NGTCP2_MILLISECONDS);
        }
        update_cached_state();
        app_->handshake_completed();
        if (server_) {
            ++endpoint_->metrics_.accepted;
            if (config.on_ready) config.on_ready(shared_from_this());
        }
    }

    // =================================================================================================
    // QuicEndpoint
    // =================================================================================================
    QuicEndpoint::QuicEndpoint(QuicEndpointConfig config)
        : strand_(asio::make_strand(reactor())), socket_(reactor()), config_(std::move(config)) {
        RAND_bytes(secret_.data(), static_cast<int>(secret_.size()));
    }

    QuicEndpoint::~QuicEndpoint() = default;

    Result<std::shared_ptr<QuicEndpoint>> QuicEndpoint::create(QuicEndpointConfig config, const udp::endpoint &bind_to) {
        std::call_once(crypto_once, [] { crypto_ready = ngtcp2_crypto_ossl_init() == 0; });
        if (!crypto_ready) return std::unexpected(IoError{IoErrorCode::Unsupported, "ngtcp2 could not initialise its OpenSSL backend"});
        std::shared_ptr<QuicEndpoint> endpoint(new QuicEndpoint(std::move(config)));
        ErrorCode ec;
        endpoint->socket_.open(bind_to.protocol(), ec);
        if (!ec && bind_to.protocol() == udp::v6() && endpoint->config_.server) endpoint->socket_.set_option(asio::ip::v6_only(false), ec);
        if (ec) return std::unexpected(to_io_error("open QUIC socket", ec));
        ErrorCode ignored;
        endpoint->socket_.set_option(asio::socket_base::receive_buffer_size(4 * 1024 * 1024), ignored);
        endpoint->socket_.set_option(asio::socket_base::send_buffer_size(4 * 1024 * 1024), ignored);
        endpoint->socket_.bind(bind_to, ec);
        if (ec) return std::unexpected(to_io_error("bind QUIC socket", ec));
        endpoint->socket_.non_blocking(true, ec);
        endpoint->local_ = endpoint->socket_.local_endpoint(ec);
        if (ec) return std::unexpected(to_io_error("query QUIC socket", ec));
        return endpoint;
    }

    void QuicEndpoint::start() {
        asio::post(strand_, [self = shared_from_this()] {
            if (self->receiving_) return; // exactly one read may be outstanding: they share one buffer
            self->receiving_ = true;
            self->receive_next();
        });
    }

    void QuicEndpoint::receive_next() {
        if (stopped_) return;
        socket_.async_receive_from(asio::buffer(receive_buffer_), sender_, asio::bind_executor(strand_, [self = shared_from_this()](const ErrorCode &ec, std::size_t size) {
            if (self->stopped_) return;
            if (ec) {
                if (ec == asio::error::operation_aborted) return;
                self->receive_next(); // e.g. ICMP-induced errors: keep serving
                return;
            }
            self->on_datagram(self->sender_, self->receive_buffer_.data(), size);
            self->receive_next();
        }));
    }

    void QuicEndpoint::send_to(const udp::endpoint &to, const std::uint8_t *data, std::size_t size) {
        if (stopped_) return;
        ErrorCode ec;
        socket_.send_to(asio::buffer(data, size), to, 0, ec); // a full send buffer just drops the packet; QUIC retransmits
    }

    void QuicEndpoint::stop() {
        asio::post(strand_, [self = shared_from_this()] {
            if (self->stopped_) return;
            std::vector<std::shared_ptr<QuicConn>> all;
            for (auto &entry : self->by_cid_) {
                if (std::find(all.begin(), all.end(), entry.second) == all.end()) all.push_back(entry.second);
            }
            if (self->client_conn_) all.push_back(self->client_conn_);
            for (auto &conn : all) conn->close_now(0, "endpoint stopped");
            self->stopped_ = true;
            ErrorCode ignored;
            self->socket_.close(ignored);
            self->by_cid_.clear();
            self->client_conn_.reset();
        });
    }

    std::string QuicEndpoint::address_key(const udp::endpoint &endpoint) const {
        auto address = endpoint.address();
        if (address.is_v6() && address.to_v6().is_v4_mapped()) address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
        return address.to_string();
    }

    void QuicEndpoint::register_cid(const std::string &cid, const std::shared_ptr<QuicConn> &conn) { by_cid_[cid] = conn; }
    void QuicEndpoint::unregister_cid(const std::string &cid) { by_cid_.erase(cid); }

    void QuicEndpoint::connection_ended(const std::shared_ptr<QuicConn> &conn) {
        for (const std::string &cid : conn->cids_) {
            if (auto it = by_cid_.find(cid); it != by_cid_.end() && it->second == conn) by_cid_.erase(it);
        }
        conn->cids_.clear();
        if (config_.server) {
            if (live_connections_ > 0) --live_connections_;
            const std::string key = address_key(conn->remote_);
            if (auto it = per_address_.find(key); it != per_address_.end() && --it->second == 0) per_address_.erase(it);
            --metrics_.active;
            if (config_.on_ended) config_.on_ended(conn, conn->handshake_done_);
        } else {
            client_conn_.reset();
            stop();
        }
    }

    Result<std::shared_ptr<QuicConn>> QuicEndpoint::connect(const udp::endpoint &remote, const std::string &host) {
        auto conn = std::shared_ptr<QuicConn>(new QuicConn(shared_from_this(), false));
        if (auto setup = conn->setup_client(remote, host); !setup) return std::unexpected(setup.error());
        conn->app_ = config_.make_app(*conn);
        client_conn_ = conn;
        metrics_.active = 1;
        // Nothing is sent yet: the caller wires up its callbacks on the app, then calls `conn->request_flush()` (and
        // `start()`) so the handshake cannot finish before it is listening.
        return conn;
    }

    void QuicEndpoint::on_datagram(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size) {
        ngtcp2_version_cid vc;
        const int rv = ngtcp2_pkt_decode_version_cid(&vc, data, size, kQuicServerCidLen);
        if (!config_.server) {
            if (client_conn_ && remote == client_conn_->remote_) client_conn_->on_packet(remote, data, size);
            else ++metrics_.dropped;
            return;
        }
        if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
            send_version_negotiation(remote, vc);
            return;
        }
        if (rv != 0) {
            ++metrics_.dropped;
            return;
        }
        if (auto it = by_cid_.find(cid_key(vc.dcid, vc.dcidlen)); it != by_cid_.end()) {
            auto conn = it->second; // (the entry may disappear while the packet is processed)
            conn->on_packet(remote, data, size);
            return;
        }
        if ((data[0] & 0x80) == 0) { // short header for a connection we do not know (e.g. after a restart)
            send_stateless_reset(remote, vc, size);
            return;
        }
        handle_initial(remote, data, size, vc);
    }

    void QuicEndpoint::send_version_negotiation(const udp::endpoint &to, const ngtcp2_version_cid &vc) {
        std::array<std::uint8_t, 256> buffer;
        const std::uint32_t versions[] = {NGTCP2_PROTO_VER_V1};
        std::uint8_t random = 0;
        RAND_bytes(&random, 1);
        const ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(buffer.data(), buffer.size(), random, vc.scid, vc.scidlen, vc.dcid, vc.dcidlen, versions, 1);
        if (n > 0) send_to(to, buffer.data(), static_cast<std::size_t>(n));
    }

    void QuicEndpoint::send_stateless_reset(const udp::endpoint &to, const ngtcp2_version_cid &vc, std::size_t received_size) {
        // Refill the bucket, then spend one: a flood of stray packets must not turn us into a traffic source.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - reset_refill_).count();
        if (elapsed >= 250) {
            reset_tokens_ = (std::min<std::uint64_t>)(16, reset_tokens_ + static_cast<std::uint64_t>(elapsed) / 62);
            reset_refill_ = now;
        }
        if (reset_tokens_ == 0 || received_size < 43) { // must stay smaller than the packet that triggered it
            ++metrics_.dropped;
            return;
        }
        --reset_tokens_;
        ngtcp2_cid cid;
        ngtcp2_cid_init(&cid, vc.dcid, vc.dcidlen);
        ngtcp2_stateless_reset_token token;
        if (ngtcp2_crypto_generate_stateless_reset_token(token.data, secret_.data(), secret_.size(), &cid) != 0) return;
        std::array<std::uint8_t, 64> random;
        RAND_bytes(random.data(), static_cast<int>(random.size()));
        const std::size_t random_length = (std::min<std::size_t>)(received_size - NGTCP2_STATELESS_RESET_TOKENLEN - 1, 32);
        std::array<std::uint8_t, 128> buffer;
        const ngtcp2_ssize n = ngtcp2_pkt_write_stateless_reset2(buffer.data(), buffer.size(), &token, random.data(), random_length);
        if (n > 0) send_to(to, buffer.data(), static_cast<std::size_t>(n));
    }

    void QuicEndpoint::refuse(const udp::endpoint &to, const ngtcp2_pkt_hd &hd, std::uint64_t transport_error) {
        std::array<std::uint8_t, 256> buffer;
        const ngtcp2_ssize n = ngtcp2_crypto_write_connection_close(buffer.data(), buffer.size(), hd.version, &hd.scid, &hd.dcid, transport_error, nullptr, 0);
        if (n > 0) send_to(to, buffer.data(), static_cast<std::size_t>(n));
    }

    void QuicEndpoint::handle_initial(const udp::endpoint &remote, const std::uint8_t *data, std::size_t size, const ngtcp2_version_cid &vc) {
        if (stopped_) return;
        ngtcp2_pkt_hd hd;
        const int accepted = ngtcp2_accept(&hd, data, size);
        if (accepted == 1) {
            send_version_negotiation(remote, vc);
            return;
        }
        if (accepted != 0) {
            ++metrics_.dropped;
            return;
        }
        const std::string address = address_key(remote);
        if (live_connections_ >= config_.max_connections || per_address_[address] >= config_.max_connections_per_address) {
            if (per_address_[address] == 0) per_address_.erase(address);
            ++metrics_.rejected_limit;
            refuse(remote, hd, NGTCP2_CONNECTION_REFUSED);
            return;
        }

        ngtcp2_cid original;
        const ngtcp2_cid *original_dcid = nullptr;
        if (config_.require_address_validation) {
            const ngtcp2_tstamp now = quic_now();
            if (hd.tokenlen == 0) {
                const ngtcp2_cid retry_scid = random_cid(kQuicServerCidLen);
                std::array<std::uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token;
                const ngtcp2_ssize token_length = ngtcp2_crypto_generate_retry_token2(token.data(), secret_.data(), secret_.size(), hd.version,
                                                                                      remote.data(), static_cast<ngtcp2_socklen>(remote.size()), &retry_scid, &hd.dcid, now);
                if (token_length < 0) return;
                std::array<std::uint8_t, 512> packet;
                const ngtcp2_ssize n = ngtcp2_crypto_write_retry(packet.data(), packet.size(), hd.version, &hd.scid, &retry_scid, &hd.dcid, token.data(),
                                                                 static_cast<std::size_t>(token_length));
                if (n < 0) return;
                ++metrics_.retries;
                send_to(remote, packet.data(), static_cast<std::size_t>(n));
                return;
            }
            if (hd.token[0] != NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2 ||
                ngtcp2_crypto_verify_retry_token2(&original, hd.token, hd.tokenlen, secret_.data(), secret_.size(), hd.version, remote.data(),
                                                  static_cast<ngtcp2_socklen>(remote.size()), &hd.dcid, 10 * NGTCP2_SECONDS, now) != 0) {
                ++metrics_.invalid_tokens;
                refuse(remote, hd, NGTCP2_INVALID_TOKEN);
                return;
            }
            original_dcid = &original;
        }

        auto conn = std::shared_ptr<QuicConn>(new QuicConn(shared_from_this(), true));
        if (auto setup = conn->setup_server(remote, hd, original_dcid); !setup) {
            ++metrics_.dropped;
            return;
        }
        conn->app_ = config_.make_app(*conn);
        ++live_connections_;
        ++per_address_[address];
        ++metrics_.active;
        conn->on_packet(remote, data, size);
    }

} // namespace SFT::Async::Detail

#endif
