#include <Async/Http3Engine.hpp>

#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC) && !defined(STURDY_PLATFORM_WEB)

#include <Async/NetCore.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <cstring>

namespace SFT::Async::Detail {

    namespace {

        std::string lower(std::string_view text) {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        /// Headers that belong to one HTTP/1.1 connection and must not cross into HTTP/3 (RFC 9114 section 4.2).
        bool connection_specific(const std::string &name) {
            return name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding" || name == "upgrade" ||
                   name == "content-length" || name == "host" || name == "te";
        }

        bool bad_header_text(const std::string &text) { return text.find_first_of(std::string("\r\n\0", 3)) != std::string::npos; }

        struct NvList {
            std::vector<std::pair<std::string, std::string>> items;
            std::vector<nghttp3_nv> view;
            void add(std::string name, std::string value) { items.emplace_back(std::move(name), std::move(value)); }
            const nghttp3_nv *finish() {
                view.clear();
                for (auto &[name, value] : items) {
                    view.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t *>(name.data()), reinterpret_cast<const std::uint8_t *>(value.data()), name.size(),
                                              value.size(), NGHTTP3_NV_FLAG_NONE});
                }
                return view.data();
            }
            [[nodiscard]] std::size_t size() const { return items.size(); }
        };

        constexpr std::uint64_t kH3NoError = 0x100;
        constexpr std::uint64_t kH3InternalError = 0x102;
        constexpr std::uint64_t kH3RequestCancelled = 0x10c;

        nghttp3_tstamp h3_now() { return quic_now(); }

    } // namespace

    struct H3Stream {
        std::int64_t id = -1;
        bool closed = false;
        bool rejected = false;
        bool responded = false;
        bool head_request = false;
        std::uint32_t header_bytes = 0;
        std::uint64_t body_bytes = 0;

        HttpRequestMessage request;   // server: being received
        HttpResponseMessage response; // client: being received

        // Outgoing body. Chunks stay put until acknowledged: nghttp3 hands pointers into them to QUIC.
        struct OutChunk {
            std::string data;
            std::uint64_t start = 0;
        };
        std::deque<OutChunk> out;
        std::uint64_t queued = 0;
        std::uint64_t handed = 0;
        std::uint64_t acked = 0;
        HttpBodyStream producer;
        CancellationSource cancel;
        bool producer_done = false;
        bool fetch_in_flight = false;

        // Client bookkeeping.
        Promise<std::expected<HttpResponseMessage, IoError>> promise;
        bool promise_done = false;
        bool cancelled = false;
        bool status_seen = false;
        std::optional<IoError> error;
        CancellationRegistration registration;

        void append(std::string data) {
            if (data.empty()) return;
            const std::uint64_t size = data.size();
            out.push_back({std::move(data), queued});
            queued += size;
        }
    };

    struct H3Callbacks {
        static H3Session *session(void *user_data) { return static_cast<H3Session *>(user_data); }
        static H3Stream *stream_of(void *stream_user_data) { return static_cast<H3Stream *>(stream_user_data); }

        static int acked_stream_data(nghttp3_conn *, std::int64_t, std::uint64_t size, void *, void *stream_user_data) {
            if (H3Stream *stream = stream_of(stream_user_data)) {
                stream->acked += size;
                while (!stream->out.empty() && stream->out.front().start + stream->out.front().data.size() <= stream->acked) stream->out.pop_front();
            }
            return 0;
        }

        static int stream_close(nghttp3_conn *, std::int64_t stream_id, std::uint64_t error_code, void *user_data, void *) {
            H3Session *s = session(user_data);
            auto it = s->streams_.find(stream_id);
            if (it == s->streams_.end()) return 0;
            std::shared_ptr<H3Stream> stream = it->second;
            s->streams_.erase(it);
            stream->closed = true;
            (void)stream->cancel.cancel(); // wakes a response producer that is waiting for events
            stream->producer = nullptr;
            if (!s->server_ && !stream->promise_done) {
                if (stream->cancelled) {
                    stream->error = IoError{IoErrorCode::Closed, "cancelled"};
                } else if (!stream->error && (error_code != kH3NoError || !stream->status_seen)) {
                    const bool refused = error_code == 0x10b; // H3_REQUEST_REJECTED
                    stream->error = IoError{refused ? IoErrorCode::ConnectionReset : IoErrorCode::ProtocolError,
                                            "HTTP/3 stream ended with error " + std::to_string(error_code)};
                }
                s->finish_stream(stream);
            }
            return 0;
        }

        static int recv_data(nghttp3_conn *, std::int64_t stream_id, const std::uint8_t *data, std::size_t size, void *user_data, void *stream_user_data) {
            H3Session *s = session(user_data);
            H3Stream *stream = stream_of(stream_user_data);
            // nghttp3 does not count DATA payload as consumed (it hands it to us here), so the flow-control credit for it is
            // ours to give back. Memory stays bounded anyway: bodies are capped below.
            ngtcp2_conn_extend_max_stream_offset(s->conn_->raw(), stream_id, size);
            ngtcp2_conn_extend_max_offset(s->conn_->raw(), size);
            if (stream == nullptr || stream->rejected) return 0;
            stream->body_bytes += size;
            if (s->server_) {
                if (stream->body_bytes > s->server_config_.max_request_body_bytes) {
                    stream->rejected = true;
                    // Answer from outside the callback; the client may stop uploading.
                    asio::post(s->conn_->strand(), [conn = s->conn_->shared_from_this(), id = stream_id, s] {
                        if (conn->closed()) return;
                        HttpResponseMessage too_big;
                        too_big.status = 413;
                        too_big.body = "Payload Too Large";
                        s->submit_response(id, std::move(too_big));
                        ngtcp2_conn_shutdown_stream_read(conn->raw(), 0, id, kH3NoError);
                        conn->request_flush();
                    });
                    return 0;
                }
                stream->request.body.append(reinterpret_cast<const char *>(data), size);
            } else {
                if (stream->body_bytes > s->client_config_.max_response_body_bytes) {
                    stream->rejected = true;
                    stream->error = IoError{IoErrorCode::LimitExceeded, "HTTP/3 response body exceeds the configured limit"};
                    asio::post(s->conn_->strand(), [conn = s->conn_->shared_from_this(), id = stream_id] {
                        if (conn->closed()) return;
                        ngtcp2_conn_shutdown_stream(conn->raw(), 0, id, kH3RequestCancelled);
                        conn->request_flush();
                    });
                    return 0;
                }
                stream->response.body.append(reinterpret_cast<const char *>(data), size);
            }
            return 0;
        }

        static int deferred_consume(nghttp3_conn *, std::int64_t stream_id, std::size_t consumed, void *user_data, void *) {
            H3Session *s = session(user_data);
            ngtcp2_conn_extend_max_stream_offset(s->conn_->raw(), stream_id, consumed);
            ngtcp2_conn_extend_max_offset(s->conn_->raw(), consumed);
            return 0;
        }

        static int begin_headers(nghttp3_conn *conn, std::int64_t stream_id, void *user_data, void *stream_user_data) {
            H3Session *s = session(user_data);
            if (s->server_ && stream_user_data == nullptr) {
                auto stream = std::make_shared<H3Stream>();
                stream->id = stream_id;
                s->streams_[stream_id] = stream;
                nghttp3_conn_set_stream_user_data(conn, stream_id, stream.get());
            }
            return 0;
        }

        static int recv_header(nghttp3_conn *, std::int64_t, std::int32_t, nghttp3_rcbuf *name, nghttp3_rcbuf *value, std::uint8_t, void *user_data,
                               void *stream_user_data) {
            H3Session *s = session(user_data);
            H3Stream *stream = stream_of(stream_user_data);
            if (stream == nullptr || stream->rejected) return 0;
            const nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
            const nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
            const std::string_view key(reinterpret_cast<const char *>(n.base), n.len);
            const std::string_view text(reinterpret_cast<const char *>(v.base), v.len);
            const std::uint32_t limit = s->server_ ? s->server_config_.max_header_bytes : s->client_config_.max_header_bytes;
            stream->header_bytes += static_cast<std::uint32_t>(n.len + v.len + 32);
            if (stream->header_bytes > limit) {
                stream->rejected = true;
                return 0;
            }
            if (s->server_) {
                if (key == ":method") stream->request.method = std::string(text);
                else if (key == ":path") stream->request.target = std::string(text);
                else if (key == ":authority") stream->request.headers.push_back({"Host", std::string(text)});
                else if (!key.empty() && key.front() != ':') {
                    if (key == "cookie") { // may be split into several fields; HTTP/1 handlers expect one
                        for (auto &h : stream->request.headers)
                            if (h.name == "cookie") {
                                h.value += "; " + std::string(text);
                                return 0;
                            }
                    }
                    stream->request.headers.push_back({std::string(key), std::string(text)});
                }
            } else {
                if (key == ":status") {
                    stream->response.status = static_cast<unsigned>(std::strtoul(std::string(text).c_str(), nullptr, 10));
                    stream->status_seen = true;
                } else if (!key.empty() && key.front() != ':') {
                    stream->response.headers.push_back({std::string(key), std::string(text)});
                }
            }
            return 0;
        }

        static int end_headers(nghttp3_conn *, std::int64_t, int, void *user_data, void *stream_user_data) {
            H3Session *s = session(user_data);
            H3Stream *stream = stream_of(stream_user_data);
            // An informational (1xx) response is not the answer: forget it and wait for the real one.
            if (stream != nullptr && !s->server_ && stream->status_seen && stream->response.status >= 100 && stream->response.status < 200) {
                stream->status_seen = false;
                stream->response.headers.clear();
                stream->header_bytes = 0;
            }
            return 0;
        }

        static int nothing_headers(nghttp3_conn *, std::int64_t, void *, void *) { return 0; }
        static int nothing_trailer(nghttp3_conn *, std::int64_t, std::int32_t, nghttp3_rcbuf *, nghttp3_rcbuf *, std::uint8_t, void *, void *) { return 0; }
        static int nothing_end_trailers(nghttp3_conn *, std::int64_t, int, void *, void *) { return 0; }

        static int end_stream(nghttp3_conn *, std::int64_t stream_id, void *user_data, void *stream_user_data) {
            H3Session *s = session(user_data);
            H3Stream *stream = stream_of(stream_user_data);
            if (stream == nullptr || !s->server_ || stream->rejected || stream->responded) return 0;
            auto it = s->streams_.find(stream_id);
            if (it != s->streams_.end()) s->request_complete(it->second);
            return 0;
        }

        static int stop_sending(nghttp3_conn *, std::int64_t stream_id, std::uint64_t error_code, void *user_data, void *) {
            ngtcp2_conn_shutdown_stream_read(session(user_data)->conn_->raw(), 0, stream_id, error_code);
            return 0;
        }

        static int reset_stream(nghttp3_conn *, std::int64_t stream_id, std::uint64_t error_code, void *user_data, void *) {
            ngtcp2_conn_shutdown_stream_write(session(user_data)->conn_->raw(), 0, stream_id, error_code);
            return 0;
        }

        static int shutdown(nghttp3_conn *, std::int64_t, void *user_data) {
            session(user_data)->usable_.store(false, std::memory_order_release); // GOAWAY: no new requests on this connection
            return 0;
        }

        static void rand(std::uint8_t *dest, std::size_t size) { RAND_bytes(dest, static_cast<int>(size)); }

        static nghttp3_ssize read_data(nghttp3_conn *, std::int64_t, nghttp3_vec *vec, std::size_t veccnt, std::uint32_t *flags, void *user_data,
                                       void *stream_user_data) {
            return session(user_data)->read_body(*stream_of(stream_user_data), vec, veccnt, flags);
        }
    };

    // =============================================================================================
    // Construction
    // =============================================================================================
    H3Session::H3Session(QuicConn &conn, bool server, H3ServerConfig server_config, H3ClientConfig client_config)
        : conn_(&conn), server_(server), server_config_(std::move(server_config)), client_config_(client_config) {}

    H3Session::~H3Session() {
        if (h3_ != nullptr) nghttp3_conn_del(h3_);
    }

    void H3Session::handshake_completed() {
        nghttp3_callbacks callbacks{};
        callbacks.acked_stream_data = H3Callbacks::acked_stream_data;
        callbacks.stream_close = H3Callbacks::stream_close;
        callbacks.recv_data = H3Callbacks::recv_data;
        callbacks.deferred_consume = H3Callbacks::deferred_consume;
        callbacks.begin_headers = H3Callbacks::begin_headers;
        callbacks.recv_header = H3Callbacks::recv_header;
        callbacks.end_headers = H3Callbacks::end_headers;
        callbacks.begin_trailers = H3Callbacks::nothing_headers;
        callbacks.recv_trailer = H3Callbacks::nothing_trailer;
        callbacks.end_trailers = H3Callbacks::nothing_end_trailers;
        callbacks.stop_sending = H3Callbacks::stop_sending;
        callbacks.end_stream = H3Callbacks::end_stream;
        callbacks.reset_stream = H3Callbacks::reset_stream;
        callbacks.shutdown = H3Callbacks::shutdown;
        callbacks.rand = H3Callbacks::rand;

        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        settings.max_field_section_size = server_ ? server_config_.max_header_bytes : client_config_.max_header_bytes;
        settings.qpack_max_dtable_capacity = 4096;
        settings.qpack_encoder_max_dtable_capacity = 4096;
        settings.qpack_blocked_streams = 16;

        const nghttp3_mem *mem = nghttp3_mem_default();
        const int created = server_ ? nghttp3_conn_server_new(&h3_, &callbacks, &settings, mem, this) : nghttp3_conn_client_new(&h3_, &callbacks, &settings, mem, this);
        if (created != 0) {
            h3_ = nullptr;
            if (on_ready) std::exchange(on_ready, nullptr)(std::unexpected(IoError{IoErrorCode::Unknown, "cannot create the HTTP/3 session"}));
            return;
        }
        // The three unidirectional streams HTTP/3 needs: control, QPACK encoder, QPACK decoder.
        std::int64_t control = -1, encoder = -1, decoder = -1;
        if (ngtcp2_conn_open_uni_stream(conn_->raw(), &control, nullptr) != 0 || ngtcp2_conn_open_uni_stream(conn_->raw(), &encoder, nullptr) != 0 ||
            ngtcp2_conn_open_uni_stream(conn_->raw(), &decoder, nullptr) != 0 || nghttp3_conn_bind_control_stream(h3_, control) != 0 ||
            nghttp3_conn_bind_qpack_streams(h3_, encoder, decoder) != 0) {
            if (on_ready) std::exchange(on_ready, nullptr)(std::unexpected(IoError{IoErrorCode::ProtocolError, "the peer allows too few unidirectional streams for HTTP/3"}));
            conn_->request_close(kH3InternalError, "cannot set up HTTP/3");
            return;
        }
        if (on_ready) std::exchange(on_ready, nullptr)({});
        open_pending_requests();
    }

    // =============================================================================================
    // QuicApp plumbing
    // =============================================================================================
    bool H3Session::recv_stream_data(std::int64_t stream_id, bool fin, const std::uint8_t *data, std::size_t size) {
        if (h3_ == nullptr) return false;
        const nghttp3_ssize consumed = nghttp3_conn_read_stream2(h3_, stream_id, data, size, fin ? 1 : 0, h3_now());
        if (consumed < 0) return false; // a protocol violation closes the connection
        ngtcp2_conn_extend_max_stream_offset(conn_->raw(), stream_id, static_cast<std::uint64_t>(consumed));
        ngtcp2_conn_extend_max_offset(conn_->raw(), static_cast<std::uint64_t>(consumed));
        return true;
    }

    bool H3Session::acked_stream_data(std::int64_t stream_id, std::uint64_t size) {
        if (h3_ == nullptr) return true;
        const int rv = nghttp3_conn_add_ack_offset(h3_, stream_id, size);
        return rv == 0 || rv == NGHTTP3_ERR_STREAM_NOT_FOUND;
    }

    void H3Session::stream_close(std::int64_t stream_id, std::uint64_t app_error_code) {
        if (h3_ == nullptr) return;
        // A stream that closed without an application error code is a clean end (HTTP/3 codes all start at 0x100).
        if (app_error_code == 0) app_error_code = kH3NoError;
        // nghttp3 finishes the stream (and calls back into `H3Callbacks::stream_close`).
        const int rv = nghttp3_conn_close_stream(h3_, stream_id, app_error_code);
        if (rv == NGHTTP3_ERR_H3_CLOSED_CRITICAL_STREAM) conn_->request_close(0x104, "critical HTTP/3 stream closed");
    }

    void H3Session::stream_reset(std::int64_t stream_id, std::uint64_t) {
        if (h3_ != nullptr) nghttp3_conn_shutdown_stream_read(h3_, stream_id);
    }

    void H3Session::stream_stop_sending(std::int64_t stream_id, std::uint64_t) {
        if (h3_ != nullptr) nghttp3_conn_shutdown_stream_read(h3_, stream_id);
    }

    void H3Session::extend_max_local_streams(bool bidirectional) {
        if (bidirectional) open_pending_requests();
    }

    void H3Session::extend_max_stream_data(std::int64_t stream_id) {
        if (h3_ != nullptr) nghttp3_conn_unblock_stream(h3_, stream_id);
    }

    bool H3Session::next_stream_write(StreamWrite &out) {
        if (h3_ == nullptr) return false;
        std::int64_t stream_id = -1;
        int fin = 0;
        nghttp3_vec vec[16];
        const nghttp3_ssize count = nghttp3_conn_writev_stream(h3_, &stream_id, &fin, vec, std::size(vec));
        if (count < 0) {
            conn_->request_close(kH3InternalError, "HTTP/3 write failed");
            return false;
        }
        if (stream_id < 0) return false;
        out.stream_id = stream_id;
        out.fin = fin != 0;
        out.veccnt = static_cast<std::size_t>(count);
        out.offered = 0;
        for (std::size_t i = 0; i < out.veccnt; ++i) {
            out.vec[i].base = vec[i].base;
            out.vec[i].len = vec[i].len;
            out.offered += vec[i].len;
        }
        return true;
    }

    void H3Session::stream_write_done(const StreamWrite &out, std::size_t bytes) {
        if (h3_ != nullptr) nghttp3_conn_add_write_offset(h3_, out.stream_id, bytes);
    }

    void H3Session::stream_write_blocked(std::int64_t stream_id) {
        if (h3_ != nullptr) nghttp3_conn_block_stream(h3_, stream_id);
    }

    void H3Session::stream_write_shut(std::int64_t stream_id) {
        if (h3_ != nullptr) nghttp3_conn_shutdown_stream_write(h3_, stream_id);
    }

    void H3Session::closed(const QuicCloseInfo &info) {
        closed_ = true;
        usable_.store(false, std::memory_order_release);
        const std::string why = info.reason.empty() ? "connection closed" : info.reason;
        if (on_ready) std::exchange(on_ready, nullptr)(std::unexpected(IoError{info.timed_out ? IoErrorCode::TimedOut : IoErrorCode::ConnectionReset, "QUIC: " + why}));
        auto streams = std::move(streams_);
        streams_.clear();
        for (auto &[id, stream] : streams) {
            stream->closed = true;
            (void)stream->cancel.cancel();
            stream->producer = nullptr;
            if (!server_ && !stream->promise_done) {
                if (!stream->error) stream->error = IoError{IoErrorCode::ConnectionReset, "HTTP/3 connection lost: " + why};
                finish_stream(stream);
            }
        }
        for (auto &queued : queued_) {
            queued.stream->error = IoError{IoErrorCode::ConnectionReset, "HTTP/3 connection lost: " + why};
            finish_stream(queued.stream);
        }
        queued_.clear();
    }

    void H3Session::finish_stream(const std::shared_ptr<H3Stream> &stream) {
        if (stream->promise_done) return;
        stream->promise_done = true;
        stream->registration.reset();
        if (stream->cancelled) {
            (void)stream->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
        } else if (stream->error) {
            (void)stream->promise.set_value(std::unexpected(*stream->error));
        } else {
            stream->response.keep_alive = true;
            (void)stream->promise.set_value(std::expected<HttpResponseMessage, IoError>(std::move(stream->response)));
        }
    }

    // =============================================================================================
    // Bodies (responses on the server, request bodies on the client)
    // =============================================================================================
    nghttp3_ssize H3Session::read_body(H3Stream &stream, nghttp3_vec *vec, std::size_t veccnt, std::uint32_t *flags) {
        std::size_t count = 0;
        for (const auto &chunk : stream.out) {
            const std::uint64_t end = chunk.start + chunk.data.size();
            if (end <= stream.handed) continue;
            if (count == veccnt) break;
            const std::uint64_t skip = stream.handed > chunk.start ? stream.handed - chunk.start : 0;
            vec[count].base = reinterpret_cast<std::uint8_t *>(const_cast<char *>(chunk.data.data())) + skip;
            vec[count].len = static_cast<std::size_t>(chunk.data.size() - skip);
            stream.handed = end;
            ++count;
        }
        const bool complete = !stream.producer || stream.producer_done;
        if (count > 0) {
            if (complete && stream.handed == stream.queued) *flags |= NGHTTP3_DATA_FLAG_EOF;
            return static_cast<nghttp3_ssize>(count);
        }
        if (complete) {
            *flags |= NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        if (auto it = streams_.find(stream.id); it != streams_.end()) start_fetch(it->second);
        return NGHTTP3_ERR_WOULDBLOCK; // resumed when the producer delivers
    }

    void H3Session::start_fetch(const std::shared_ptr<H3Stream> &stream) {
        if (stream->fetch_in_flight || !stream->producer) return;
        stream->fetch_in_flight = true;
        TaskHandle<std::optional<std::string>> pending;
        try {
            pending = stream->producer(stream->cancel.token());
        } catch (...) {
            stream->fetch_in_flight = false;
            ngtcp2_conn_shutdown_stream(conn_->raw(), 0, stream->id, kH3InternalError);
            return;
        }
        pending.on_done([conn = conn_->shared_from_this(), this, stream, pending]() mutable {
            asio::post(conn->strand(), [conn, this, stream, pending]() mutable {
                stream->fetch_in_flight = false;
                if (stream->closed || closed_) return;
                try {
                    auto item = pending.wait();
                    if (!item) stream->producer_done = true;
                    else stream->append(std::move(*item));
                } catch (...) { // the body failed after the headers went out: abort just this stream
                    ngtcp2_conn_shutdown_stream(conn->raw(), 0, stream->id, kH3InternalError);
                    conn->request_flush();
                    return;
                }
                nghttp3_conn_resume_stream(h3_, stream->id);
                conn->request_flush();
            });
        });
    }

    // =============================================================================================
    // Server: requests in, responses out
    // =============================================================================================
    void H3Session::request_complete(const std::shared_ptr<H3Stream> &stream) {
        stream->responded = true; // exactly one dispatch per stream
        stream->request.version = 30;
        stream->request.remote_address = server_config_.remote_address;
        stream->head_request = stream->request.method == "HEAD";
        const std::int64_t id = stream->id;
        server_config_.dispatch(std::move(stream->request), [conn = conn_->shared_from_this(), this, id](HttpResponseMessage response) {
            asio::post(conn->strand(), [conn, this, id, response = std::move(response)]() mutable { submit_response(id, std::move(response)); });
        });
    }

    void H3Session::submit_response(std::int64_t id, HttpResponseMessage response) {
        auto it = streams_.find(id);
        if (it == streams_.end() || closed_ || h3_ == nullptr) return; // the client reset the stream or the connection died meanwhile
        std::shared_ptr<H3Stream> stream = it->second;
        if (server_config_.on_response) server_config_.on_response(response.status);

        bool invalid = response.status < 100 || response.status > 599;
        for (const HttpHeader &h : response.headers) {
            if (h.name.empty() || bad_header_text(h.name) || bad_header_text(h.value) || h.name.find(':') != std::string::npos) invalid = true;
        }
        const bool use_file = !response.body_file.empty();
        if (!use_file && !response.body_stream && response.body.size() > server_config_.max_response_body_bytes) invalid = true;
        if (invalid) {
            response = HttpResponseMessage{};
            response.status = 500;
            response.body = "Internal Server Error";
        }

        const unsigned status = response.status;
        const bool no_body = stream->head_request || status == 204 || status == 304 || (status >= 100 && status < 200);
        std::optional<std::uint64_t> content_length;
        bool has_body = false;
        if (use_file) {
            content_length = response.body_length.value_or(0);
            if (!no_body && *content_length > 0) {
                stream->producer = make_file_producer(response.body_file, response.body_offset, *content_length);
                has_body = true;
            }
        } else if (response.body_stream) {
            if (!no_body) {
                stream->producer = std::move(response.body_stream);
                has_body = true;
            }
        } else {
            content_length = response.body.size();
            if (!no_body && !response.body.empty()) {
                stream->append(std::move(response.body));
                has_body = true;
            }
        }

        NvList headers;
        headers.add(":status", std::to_string(status));
        for (const HttpHeader &h : response.headers) {
            std::string name = lower(h.name);
            if (connection_specific(name)) {
                if (name == "content-length" && stream->head_request) content_length = std::strtoull(h.value.c_str(), nullptr, 10);
                continue;
            }
            headers.add(std::move(name), h.value);
        }
        if (content_length && status != 204 && status != 304) headers.add("content-length", std::to_string(*content_length));
        headers.add("server", "SturdyEngine Async HTTP/3");

        nghttp3_data_reader reader{H3Callbacks::read_data};
        const nghttp3_nv *nv = headers.finish();
        const int rv = nghttp3_conn_submit_response(h3_, id, nv, headers.size(), has_body ? &reader : nullptr);
        if (rv != 0) {
            ngtcp2_conn_shutdown_stream(conn_->raw(), 0, id, kH3InternalError);
        }
        conn_->request_flush();
    }

    // =============================================================================================
    // Client: requests out, responses in
    // =============================================================================================
    TaskHandle<std::expected<HttpResponseMessage, IoError>> H3Session::submit(HttpRequestMessage request, std::string scheme, std::string authority,
                                                                              CancellationToken cancellation) {
        Promise<std::expected<HttpResponseMessage, IoError>> promise;
        auto handle = promise.handle();
        asio::post(conn_->strand(), [conn = conn_->shared_from_this(), this, request = std::move(request), scheme = std::move(scheme),
                                     authority = std::move(authority), cancellation, promise]() mutable {
            if (closed_ || !usable_.load(std::memory_order_acquire) || h3_ == nullptr) {
                (void)promise.set_value(std::unexpected(IoError{IoErrorCode::ConnectionReset, "HTTP/3 connection is not accepting new requests"}));
                return;
            }
            if (cancellation.is_cancelled()) {
                (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                return;
            }
            auto stream = std::make_shared<H3Stream>();
            stream->promise = promise;
            if (cancellation.can_be_cancelled()) {
                stream->registration = cancellation.on_cancel([weak = std::weak_ptr<H3Stream>(stream), conn, this] {
                    asio::post(conn->strand(), [weak, conn, this] {
                        auto s = weak.lock();
                        if (!s || s->promise_done) return;
                        s->cancelled = true;
                        if (s->id >= 0 && !s->closed) {
                            ngtcp2_conn_shutdown_stream(conn->raw(), 0, s->id, kH3RequestCancelled);
                            conn->request_flush();
                        } else if (s->id < 0) { // still queued: never sent
                            std::erase_if(queued_, [&](const QueuedRequest &q) { return q.stream == s; });
                            s->closed = true;
                            finish_stream(s);
                        }
                    });
                });
            }
            if (!start_request(stream, request, scheme, authority)) {
                queued_.push_back({stream, std::move(request), std::move(scheme), std::move(authority)});
                conn->request_flush(); // lets QUIC send STREAMS_BLOCKED
            }
        });
        return handle;
    }

    /// False: the server's stream limit is reached right now (the request waits in `queued_`).
    bool H3Session::start_request(const std::shared_ptr<H3Stream> &stream, HttpRequestMessage &request, const std::string &scheme, const std::string &authority) {
        std::int64_t id = -1;
        const int opened = ngtcp2_conn_open_bidi_stream(conn_->raw(), &id, nullptr);
        if (opened == NGTCP2_ERR_STREAM_ID_BLOCKED) return false;
        if (opened != 0) {
            stream->error = IoError{IoErrorCode::ConnectionReset, std::string("cannot open an HTTP/3 stream: ") + ngtcp2_strerror(opened)};
            finish_stream(stream);
            return true;
        }
        stream->id = id;
        NvList headers;
        headers.add(":method", request.method);
        headers.add(":scheme", scheme);
        headers.add(":authority", authority);
        headers.add(":path", request.target.empty() ? "/" : request.target);
        for (const HttpHeader &h : request.headers) {
            std::string name = lower(h.name);
            if (connection_specific(name)) continue;
            headers.add(std::move(name), h.value);
        }
        const bool has_body = !request.body.empty();
        if (has_body || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") headers.add("content-length", std::to_string(request.body.size()));
        if (has_body) stream->append(std::move(request.body));
        nghttp3_data_reader reader{H3Callbacks::read_data};
        streams_[id] = stream;
        const int rv = nghttp3_conn_submit_request(h3_, id, headers.finish(), headers.size(), has_body ? &reader : nullptr, stream.get());
        if (rv != 0) {
            streams_.erase(id);
            stream->error = IoError{IoErrorCode::ProtocolError, std::string("cannot submit the HTTP/3 request: ") + nghttp3_strerror(rv)};
            ngtcp2_conn_shutdown_stream(conn_->raw(), 0, id, kH3InternalError);
            finish_stream(stream);
            return true;
        }
        conn_->request_flush();
        return true;
    }

    void H3Session::open_pending_requests() {
        while (!queued_.empty() && h3_ != nullptr) {
            QueuedRequest &front = queued_.front();
            if (front.stream->cancelled || front.stream->promise_done) {
                queued_.pop_front();
                continue;
            }
            if (!start_request(front.stream, front.request, front.scheme, front.authority)) break;
            queued_.pop_front();
        }
    }

    // =============================================================================================
    // Setup helpers
    // =============================================================================================
    Result<std::shared_ptr<QuicEndpoint>> start_h3_server(const udp::endpoint &bind_to, TlsServerOptions tls, H3ServerConfig config, std::uint32_t max_concurrent_streams,
                                                          std::size_t max_connections, std::size_t max_connections_per_address) {
        tls.alpn = {"h3"};
        auto context = make_tls_server_context(tls);
        if (!context) return std::unexpected(context.error());
        harden_quic_context((*context)->native_handle(), true);

        QuicEndpointConfig endpoint_config;
        endpoint_config.server = true;
        endpoint_config.limits.stream_receive_window = 1 << 20;
        endpoint_config.limits.connection_receive_window = 16 << 20;
        endpoint_config.limits.max_bidirectional_streams = max_concurrent_streams;
        endpoint_config.limits.max_unidirectional_streams = 16;
        endpoint_config.limits.datagrams = false;
        endpoint_config.tls_context = *context;
        endpoint_config.pinned_client_sha256 = tls.pinned_client_sha256;
        endpoint_config.max_connections = max_connections;
        endpoint_config.max_connections_per_address = max_connections_per_address;
        endpoint_config.make_app = [config = std::move(config)](QuicConn &conn) -> std::shared_ptr<QuicApp> {
            H3ServerConfig per_connection = config;
            per_connection.remote_address = to_socket_address(conn.remote()).host;
            return std::make_shared<H3Session>(conn, true, std::move(per_connection), H3ClientConfig{});
        };
        auto endpoint = QuicEndpoint::create(std::move(endpoint_config), bind_to);
        if (!endpoint) return std::unexpected(endpoint.error());
        (*endpoint)->start();
        return endpoint;
    }

    TaskHandle<Result<std::shared_ptr<H3Client>>> connect_h3(udp::endpoint address, std::string host, TlsClientOptions tls, H3ClientConfig config,
                                                             std::chrono::milliseconds handshake_timeout) {
        using Out = Result<std::shared_ptr<H3Client>>;
        Promise<Out> promise;
        auto handle = promise.handle();
        auto fail = [&](IoError error) {
            (void)promise.set_value(Out(std::unexpected(std::move(error))));
            return handle;
        };
        tls.alpn = {"h3"};
        auto context = make_tls_client_context(tls);
        if (!context) return fail(context.error());
        harden_quic_context((*context)->native_handle(), false);

        QuicEndpointConfig endpoint_config;
        endpoint_config.server = false;
        endpoint_config.limits.stream_receive_window = 1 << 20;
        endpoint_config.limits.connection_receive_window = 16 << 20;
        endpoint_config.limits.max_bidirectional_streams = 0;
        endpoint_config.limits.max_unidirectional_streams = 16;
        endpoint_config.limits.datagrams = false;
        endpoint_config.limits.handshake_timeout = handshake_timeout;
        endpoint_config.tls_context = *context;
        endpoint_config.client_tls = tls;
        endpoint_config.make_app = [config](QuicConn &conn) -> std::shared_ptr<QuicApp> {
            return std::make_shared<H3Session>(conn, false, H3ServerConfig{}, config);
        };
        auto endpoint = QuicEndpoint::create(std::move(endpoint_config), udp::endpoint(address.protocol(), 0));
        if (!endpoint) return fail(endpoint.error());
        auto conn = (*endpoint)->connect(address, host);
        if (!conn) return fail(conn.error());
        auto client = std::make_shared<H3Client>();
        client->conn = *conn;
        client->session = static_cast<H3Session *>((*conn)->app());
        client->session->on_ready = [promise, client](Result<void> result) mutable {
            if (result) (void)promise.set_value(Out(client));
            else (void)promise.set_value(Out(std::unexpected(result.error())));
        };
        (*endpoint)->start();
        client->conn->request_flush(); // begins the handshake, now that on_ready is in place
        return handle;
    }

} // namespace SFT::Async::Detail

#endif
