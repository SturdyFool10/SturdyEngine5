#include <Async/Http2Engine.hpp>

#if defined(STURDY_ASYNC_HTTP2)

#include <cstring>

namespace SFT::Async::Detail {

    namespace {

        constexpr std::uint32_t kInitialWindow = 1u << 20;       // per-stream receive window
        constexpr std::uint32_t kConnectionWindow = 16u << 20;   // connection-level receive window

        std::string lower(std::string_view text) {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        /// Headers that belong to one HTTP/1.1 connection and must not cross into HTTP/2 (RFC 9113 section 8.2.2).
        bool connection_specific(const std::string &lowered_name) {
            return lowered_name == "connection" || lowered_name == "keep-alive" || lowered_name == "proxy-connection" || lowered_name == "transfer-encoding" ||
                   lowered_name == "upgrade" || lowered_name == "content-length" || lowered_name == "host";
        }

        bool bad_header_text(const std::string &text) { return text.find_first_of(std::string("\r\n\0", 3)) != std::string::npos; }

        /// Collects name/value pairs and exposes them as an `nghttp2_nv` array (nghttp2 copies them at submit time).
        struct NvList {
            std::vector<std::pair<std::string, std::string>> items;
            std::vector<nghttp2_nv> view;
            void add(std::string name, std::string value) { items.emplace_back(std::move(name), std::move(value)); }
            const nghttp2_nv *finish() {
                view.clear();
                for (auto &[name, value] : items) {
                    view.push_back(nghttp2_nv{reinterpret_cast<uint8_t *>(name.data()), reinterpret_cast<uint8_t *>(value.data()), name.size(), value.size(),
                                              NGHTTP2_NV_FLAG_NONE});
                }
                return view.data();
            }
            [[nodiscard]] std::size_t size() const { return items.size(); }
        };

    } // namespace

    struct H2Stream {
        int32_t id = 0;
        bool closed = false;
        bool rejected = false;        ///< over a limit or malformed: ignore the rest of the stream
        bool responded = false;
        bool head_request = false;
        std::uint32_t header_bytes = 0;
        std::uint64_t body_bytes = 0;

        HttpRequestMessage request;   // server: being received
        HttpResponseMessage response; // client: being received

        // Outgoing body (a server response, or a client request body).
        std::vector<std::byte> out_body;
        std::size_t out_offset = 0;
        HttpBodyStream producer;
        CancellationSource cancel;
        std::deque<std::string> pending;
        std::size_t pending_offset = 0;
        bool producer_done = false;
        bool fetch_in_flight = false;

        // Client bookkeeping.
        Promise<std::expected<HttpResponseMessage, IoError>> promise;
        bool promise_done = false;
        bool cancelled = false;
        bool status_seen = false;
        std::optional<IoError> error;
        CancellationRegistration registration;
    };

    struct H2Callbacks {
        static H2Engine *engine(void *user_data) { return static_cast<H2Engine *>(user_data); }
        static H2Stream *stream_of(nghttp2_session *session, int32_t id) { return static_cast<H2Stream *>(nghttp2_session_get_stream_user_data(session, id)); }

        static int on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
            H2Engine *e = engine(user_data);
            if (frame->hd.type == NGHTTP2_HEADERS && e->server_ && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
                auto stream = std::make_shared<H2Stream>();
                stream->id = frame->hd.stream_id;
                e->streams_[stream->id] = stream;
                nghttp2_session_set_stream_user_data(session, stream->id, stream.get());
            }
            return 0;
        }

        static int on_header(nghttp2_session *session, const nghttp2_frame *frame, nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t, void *user_data) {
            H2Engine *e = engine(user_data);
            H2Stream *stream = stream_of(session, frame->hd.stream_id);
            if (stream == nullptr || stream->rejected) return 0;
            const nghttp2_vec n = nghttp2_rcbuf_get_buf(name);
            const nghttp2_vec v = nghttp2_rcbuf_get_buf(value);
            const std::string_view key(reinterpret_cast<const char *>(n.base), n.len);
            const std::string_view text(reinterpret_cast<const char *>(v.base), v.len);
            const std::uint32_t limit = e->server_ ? e->server_config_.max_header_bytes : e->client_config_.max_header_bytes;
            stream->header_bytes += static_cast<std::uint32_t>(n.len + v.len + 32); // 32 bytes of HPACK entry overhead, as the RFC counts
            if (stream->header_bytes > limit) {
                stream->rejected = true;
                return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE; // resets just this stream
            }
            if (e->server_) {
                if (key == ":method") stream->request.method = std::string(text);
                else if (key == ":path") stream->request.target = std::string(text);
                else if (key == ":authority") stream->request.headers.push_back({"Host", std::string(text)});
                else if (!key.empty() && key.front() != ':') {
                    if (key == "cookie") { // HTTP/2 may split cookies into several fields; HTTP/1 handlers expect one
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

        static int on_data_chunk(nghttp2_session *session, uint8_t, int32_t stream_id, const uint8_t *data, size_t length, void *user_data) {
            H2Engine *e = engine(user_data);
            H2Stream *stream = stream_of(session, stream_id);
            if (stream == nullptr || stream->rejected) return 0;
            stream->body_bytes += length;
            if (e->server_) {
                if (stream->body_bytes > e->server_config_.max_request_body_bytes) {
                    stream->rejected = true;
                    HttpResponseMessage too_big;
                    too_big.status = 413;
                    too_big.body = "Payload Too Large";
                    e->submit_response(stream_id, std::move(too_big));
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_NO_ERROR); // the client may stop uploading
                    return 0;
                }
                stream->request.body.append(reinterpret_cast<const char *>(data), length);
            } else {
                if (stream->body_bytes > e->client_config_.max_response_body_bytes) {
                    stream->rejected = true;
                    stream->error = IoError{IoErrorCode::LimitExceeded, "HTTP/2 response body exceeds the configured limit"};
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
                    return 0;
                }
                stream->response.body.append(reinterpret_cast<const char *>(data), length);
            }
            return 0;
        }

        static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
            H2Engine *e = engine(user_data);
            if (frame->hd.type == NGHTTP2_GOAWAY) {
                e->usable_.store(false, std::memory_order_release);
                return 0;
            }
            const bool carries_message = frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS;
            if (carries_message && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && e->server_) {
                auto it = e->streams_.find(frame->hd.stream_id);
                if (it != e->streams_.end() && !it->second->rejected && !it->second->responded) e->request_complete(it->second);
            }
            (void)session;
            return 0;
        }

        static int on_stream_close(nghttp2_session *, int32_t stream_id, uint32_t error_code, void *user_data) {
            H2Engine *e = engine(user_data);
            auto it = e->streams_.find(stream_id);
            if (it == e->streams_.end()) return 0;
            std::shared_ptr<H2Stream> stream = it->second;
            e->streams_.erase(it);
            stream->closed = true;
            (void)stream->cancel.cancel(); // wakes a response producer that is waiting for events
            stream->producer = nullptr;
            if (!e->server_ && !stream->promise_done) {
                if (stream->cancelled) {
                    stream->error = IoError{IoErrorCode::Closed, "cancelled"};
                } else if (!stream->error && (error_code != 0 || !stream->status_seen)) {
                    const bool refused = error_code == NGHTTP2_REFUSED_STREAM;
                    stream->error = IoError{refused ? IoErrorCode::ConnectionReset : IoErrorCode::ProtocolError,
                                            std::string("HTTP/2 stream ended with error ") + std::to_string(error_code)};
                }
                e->finished_.push_back(stream);
            }
            return 0;
        }

        static nghttp2_ssize read_body(nghttp2_session *, int32_t, uint8_t *buffer, size_t length, uint32_t *flags, nghttp2_data_source *source, void *user_data) {
            return engine(user_data)->read_body(*static_cast<H2Stream *>(source->ptr), buffer, length, flags);
        }
    };

    // =============================================================================================
    // Construction
    // =============================================================================================
    H2Engine::H2Engine(bool server, std::shared_ptr<H2Transport> transport) : server_(server), transport_(std::move(transport)) {
        nghttp2_session_callbacks *callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &H2Callbacks::on_begin_headers);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &H2Callbacks::on_data_chunk);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &H2Callbacks::on_frame_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &H2Callbacks::on_stream_close);
        nghttp2_session_callbacks_set_on_header_callback2(callbacks, &H2Callbacks::on_header);

        nghttp2_option *option = nullptr;
        nghttp2_option_new(&option);
        // Hardening against the known HTTP/2 denial-of-service families.
        nghttp2_option_set_stream_reset_rate_limit(option, 1000, 33);   // rapid reset (CVE-2023-44487)
        nghttp2_option_set_max_continuations(option, 8);                // CONTINUATION flood (CVE-2024-27983 family)
        nghttp2_option_set_max_outbound_ack(option, 1000);              // SETTINGS/PING ack floods
        nghttp2_option_set_max_settings(option, 32);
        nghttp2_option_set_glitch_rate_limit(option, 10000, 33);        // generic protocol-misbehaviour budget
        nghttp2_option_set_max_send_header_block_length(option, 256 * 1024);
        if (server) nghttp2_session_server_new3(&session_, callbacks, this, option, nullptr);
        else nghttp2_session_client_new3(&session_, callbacks, this, option, nullptr);
        nghttp2_option_del(option);
        nghttp2_session_callbacks_del(callbacks);
    }

    H2Engine::~H2Engine() {
        if (session_ != nullptr) nghttp2_session_del(session_);
    }

    std::shared_ptr<H2Engine> H2Engine::make_server(std::shared_ptr<H2Transport> transport, H2ServerConfig config, std::function<void()> on_closed) {
        std::shared_ptr<H2Engine> engine(new H2Engine(true, std::move(transport)));
        engine->server_config_ = std::move(config);
        engine->on_closed_ = std::move(on_closed);
        return engine;
    }

    std::shared_ptr<H2Engine> H2Engine::make_client(std::shared_ptr<H2Transport> transport, H2ClientConfig config, std::function<void()> on_closed) {
        std::shared_ptr<H2Engine> engine(new H2Engine(false, std::move(transport)));
        engine->client_config_ = config;
        engine->on_closed_ = std::move(on_closed);
        return engine;
    }

    void H2Engine::start() {
        asio::post(transport_->executor(), [self = shared_from_this()] {
            if (self->started_ || self->closed_) return;
            self->started_ = true;
            std::vector<nghttp2_settings_entry> settings;
            settings.push_back({NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, kInitialWindow});
            if (self->server_) {
                settings.push_back({NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, self->server_config_.max_concurrent_streams});
                settings.push_back({NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, self->server_config_.max_header_bytes});
            } else {
                settings.push_back({NGHTTP2_SETTINGS_ENABLE_PUSH, 0}); // we never want server push
                settings.push_back({NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, self->client_config_.max_header_bytes});
            }
            nghttp2_submit_settings(self->session_, NGHTTP2_FLAG_NONE, settings.data(), settings.size());
            nghttp2_session_set_local_window_size(self->session_, NGHTTP2_FLAG_NONE, 0, static_cast<int32_t>(kConnectionWindow));
            self->pump_write();
            self->pump_read();
        });
    }

    void H2Engine::close() {
        asio::post(transport_->executor(), [self = shared_from_this()] { self->fail_connection("closed"); });
    }

    // =============================================================================================
    // IO pumps
    // =============================================================================================
    void H2Engine::pump_read() {
        if (reading_ || closed_) return;
        reading_ = true;
        transport_->read_some([self = shared_from_this()](bool ok, const std::byte *data, std::size_t size) {
            self->reading_ = false;
            if (self->closed_) return;
            if (!ok) return self->fail_connection("connection closed by peer");
            const nghttp2_ssize consumed = nghttp2_session_mem_recv2(self->session_, reinterpret_cast<const uint8_t *>(data), size);
            if (consumed < 0) return self->fail_connection(std::string("HTTP/2 protocol error: ") + nghttp2_strerror(static_cast<int>(consumed)));
            self->drain_finished();
            self->pump_write();
            if (!self->closed_) self->pump_read();
        });
    }

    void H2Engine::pump_write() {
        if (writing_ || closed_) return;
        std::vector<std::byte> out;
        for (;;) {
            const uint8_t *chunk = nullptr;
            const nghttp2_ssize n = nghttp2_session_mem_send2(session_, &chunk);
            if (n < 0) return fail_connection(std::string("HTTP/2 send error: ") + nghttp2_strerror(static_cast<int>(n)));
            if (n == 0) break;
            out.insert(out.end(), reinterpret_cast<const std::byte *>(chunk), reinterpret_cast<const std::byte *>(chunk) + n);
            if (out.size() >= 64 * 1024) break;
        }
        drain_finished();
        if (out.empty()) {
            // Nothing to send and nothing more to receive: the connection has been shut down cleanly.
            if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_)) fail_connection("connection finished");
            return;
        }
        writing_ = true;
        transport_->write(std::move(out), [self = shared_from_this()](bool ok) {
            self->writing_ = false;
            if (!ok) return self->fail_connection("write failed");
            self->pump_write();
        });
    }

    void H2Engine::drain_finished() {
        if (finished_.empty()) return;
        auto done = std::move(finished_);
        finished_.clear();
        for (auto &stream : done) {
            if (stream->promise_done) continue;
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
    }

    void H2Engine::fail_connection(const std::string &why) {
        if (closed_) return;
        closed_ = true;
        usable_.store(false, std::memory_order_release);
        for (auto &[id, stream] : streams_) {
            stream->closed = true;
            (void)stream->cancel.cancel();
            stream->producer = nullptr;
            if (!server_ && !stream->promise_done) {
                if (!stream->error) stream->error = IoError{IoErrorCode::ConnectionReset, "HTTP/2 connection lost: " + why};
                finished_.push_back(stream);
            }
        }
        streams_.clear();
        drain_finished();
        transport_->close();
        if (on_closed_) std::exchange(on_closed_, nullptr)();
    }

    // =============================================================================================
    // Bodies (responses on the server, request bodies on the client)
    // =============================================================================================
    nghttp2_ssize H2Engine::read_body(H2Stream &stream, uint8_t *buffer, size_t length, uint32_t *flags) {
        if (!stream.producer) { // fully buffered
            const std::size_t n = (std::min)(length, stream.out_body.size() - stream.out_offset);
            if (n > 0) std::memcpy(buffer, stream.out_body.data() + stream.out_offset, n);
            stream.out_offset += n;
            if (stream.out_offset >= stream.out_body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<nghttp2_ssize>(n);
        }
        if (stream.pending.empty()) {
            if (stream.producer_done) {
                *flags |= NGHTTP2_DATA_FLAG_EOF;
                return 0;
            }
            auto it = streams_.find(stream.id);
            if (it != streams_.end()) start_fetch(it->second);
            return NGHTTP2_ERR_DEFERRED; // resumed when the producer delivers
        }
        std::size_t copied = 0;
        while (copied < length && !stream.pending.empty()) {
            const std::string &front = stream.pending.front();
            const std::size_t take = (std::min)(length - copied, front.size() - stream.pending_offset);
            std::memcpy(buffer + copied, front.data() + stream.pending_offset, take);
            copied += take;
            stream.pending_offset += take;
            if (stream.pending_offset == front.size()) {
                stream.pending.pop_front();
                stream.pending_offset = 0;
            }
        }
        if (stream.pending.empty() && stream.producer_done) *flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<nghttp2_ssize>(copied);
    }

    void H2Engine::start_fetch(const std::shared_ptr<H2Stream> &stream) {
        if (stream->fetch_in_flight || !stream->producer) return;
        stream->fetch_in_flight = true;
        TaskHandle<std::optional<std::string>> pending;
        try {
            pending = stream->producer(stream->cancel.token());
        } catch (...) {
            stream->fetch_in_flight = false;
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream->id, NGHTTP2_INTERNAL_ERROR);
            return;
        }
        pending.on_done([self = shared_from_this(), stream, pending]() mutable {
            asio::post(self->transport_->executor(), [self, stream, pending]() mutable {
                stream->fetch_in_flight = false;
                if (stream->closed || self->closed_) return;
                try {
                    auto item = pending.wait();
                    if (!item) stream->producer_done = true;
                    else if (!item->empty()) stream->pending.push_back(std::move(*item));
                } catch (...) { // the body failed after the headers went out: abort just this stream
                    nghttp2_submit_rst_stream(self->session_, NGHTTP2_FLAG_NONE, stream->id, NGHTTP2_INTERNAL_ERROR);
                    self->pump_write();
                    return;
                }
                nghttp2_session_resume_data(self->session_, stream->id);
                self->pump_write();
            });
        });
    }

    // =============================================================================================
    // Server: requests in, responses out
    // =============================================================================================
    void H2Engine::request_complete(const std::shared_ptr<H2Stream> &stream) {
        stream->responded = true; // exactly one dispatch per stream
        stream->request.version = 20;
        stream->request.remote_address = server_config_.remote_address;
        stream->head_request = stream->request.method == "HEAD";
        const int32_t id = stream->id;
        server_config_.dispatch(std::move(stream->request), [self = shared_from_this(), id](HttpResponseMessage response) {
            asio::post(self->transport_->executor(), [self, id, response = std::move(response)]() mutable { self->submit_response(id, std::move(response)); });
        });
    }

    void H2Engine::submit_response(int32_t id, HttpResponseMessage response) {
        auto it = streams_.find(id);
        if (it == streams_.end() || closed_) return; // the client reset the stream or the connection died meanwhile
        std::shared_ptr<H2Stream> stream = it->second;
        if (server_config_.on_response) server_config_.on_response(response.status);

        // Anything that would corrupt the message is refused: same rules as the HTTP/1.1 path.
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
        nghttp2_data_provider2 provider{};
        provider.read_callback = &H2Callbacks::read_body;
        provider.source.ptr = stream.get();
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
                stream->out_body.resize(response.body.size());
                std::memcpy(stream->out_body.data(), response.body.data(), response.body.size());
                has_body = true;
            }
        }

        NvList headers;
        headers.add(":status", std::to_string(status));
        for (const HttpHeader &h : response.headers) {
            std::string name = lower(h.name);
            if (connection_specific(name)) {
                // A HEAD handler may declare the length of the body it is not sending.
                if (name == "content-length" && stream->head_request) content_length = std::strtoull(h.value.c_str(), nullptr, 10);
                continue;
            }
            headers.add(std::move(name), h.value);
        }
        if (content_length && status != 204 && status != 304) headers.add("content-length", std::to_string(*content_length));
        headers.add("server", "SturdyEngine Async HTTP/2");

        const nghttp2_nv *nv = headers.finish();
        const int rv = nghttp2_submit_response2(session_, id, nv, headers.size(), has_body ? &provider : nullptr);
        if (rv != 0) {
            fail_connection(std::string("cannot submit response: ") + nghttp2_strerror(rv));
            return;
        }
        pump_write();
    }

    // =============================================================================================
    // Client: requests out, responses in
    // =============================================================================================
    TaskHandle<std::expected<HttpResponseMessage, IoError>> H2Engine::submit(HttpRequestMessage request, std::string scheme, std::string authority,
                                                                             CancellationToken cancellation) {
        Promise<std::expected<HttpResponseMessage, IoError>> promise;
        auto handle = promise.handle();
        asio::post(transport_->executor(), [self = shared_from_this(), request = std::move(request), scheme = std::move(scheme), authority = std::move(authority),
                                            cancellation, promise]() mutable {
            auto fail = [&](IoError error) { (void)promise.set_value(std::unexpected(std::move(error))); };
            if (self->closed_ || !self->usable_.load(std::memory_order_acquire)) {
                return fail(IoError{IoErrorCode::ConnectionReset, "HTTP/2 connection is not accepting new requests"});
            }
            if (cancellation.is_cancelled()) return (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));

            NvList headers;
            headers.add(":method", request.method);
            headers.add(":scheme", scheme);
            headers.add(":authority", authority);
            headers.add(":path", request.target.empty() ? "/" : request.target);
            for (const HttpHeader &h : request.headers) {
                std::string name = lower(h.name);
                if (connection_specific(name) || name == "te") continue;
                headers.add(std::move(name), h.value);
            }
            if (!request.body.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") {
                headers.add("content-length", std::to_string(request.body.size()));
            }

            auto stream = std::make_shared<H2Stream>();
            stream->promise = promise;
            nghttp2_data_provider2 provider{};
            provider.read_callback = &H2Callbacks::read_body;
            provider.source.ptr = stream.get();
            const bool has_body = !request.body.empty();
            if (has_body) {
                stream->out_body.resize(request.body.size());
                std::memcpy(stream->out_body.data(), request.body.data(), request.body.size());
            }
            const int32_t id = nghttp2_submit_request2(self->session_, nullptr, headers.finish(), headers.size(), has_body ? &provider : nullptr, stream.get());
            if (id < 0) return fail(IoError{IoErrorCode::ProtocolError, std::string("cannot submit request: ") + nghttp2_strerror(id)});
            stream->id = id;
            self->streams_[id] = stream;
            if (cancellation.can_be_cancelled()) {
                stream->registration = cancellation.on_cancel([weak = std::weak_ptr<H2Engine>(self), id] {
                    auto engine = weak.lock();
                    if (!engine) return;
                    asio::post(engine->transport_->executor(), [engine, id] {
                        auto it = engine->streams_.find(id);
                        if (it == engine->streams_.end() || engine->closed_) return;
                        it->second->cancelled = true;
                        nghttp2_submit_rst_stream(engine->session_, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
                        engine->pump_write();
                    });
                });
            }
            self->pump_write();
        });
        return handle;
    }

} // namespace SFT::Async::Detail

#endif
