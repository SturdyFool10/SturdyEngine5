#pragma once

// Private: an HTTP/2 connection (RFC 9113) on top of nghttp2, usable as server or client over any ordered byte transport
// (a TLS stream negotiated with ALPN "h2"). Framing, HPACK, flow control and the stream state machine are nghttp2's;
// this layer adds the engine glue, limits, streaming bodies and the mapping to Async's HTTP types.
//
// Everything runs on the transport's executor, which must be a strand (or single-threaded): the public entry points
// post onto it, so they are safe to call from any thread.

#if defined(STURDY_ASYNC_HTTP2)

#include <Async/HttpInternal.hpp>

#include <nghttp2/nghttp2.h>

#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace SFT::Async::Detail {

    /// What the engine needs from the connection underneath. All completion callbacks must run on `executor()`.
    struct H2Transport {
        virtual ~H2Transport() = default;
        [[nodiscard]] virtual asio::any_io_executor executor() = 0;
        /// Delivers the next chunk of bytes; `ok == false` at end of stream or on error.
        virtual void read_some(std::function<void(bool ok, const std::byte *data, std::size_t size)> done) = 0;
        virtual void write(std::vector<std::byte> data, std::function<void(bool ok)> done) = 0;
        virtual void close() = 0;
    };

    struct H2ServerConfig {
        std::uint64_t max_request_body_bytes = 1 << 20;
        std::uint32_t max_header_bytes = 32 * 1024;
        std::uint64_t max_response_body_bytes = 8 << 20;
        std::uint32_t max_concurrent_streams = 100;
        std::string remote_address;
        /// Hands a request to the application; `respond` must be called exactly once, from any thread.
        std::function<void(HttpRequestMessage, std::function<void(HttpResponseMessage)> respond)> dispatch;
        std::function<void(unsigned status)> on_response;   ///< metrics hook
    };

    struct H2ClientConfig {
        std::uint64_t max_response_body_bytes = 16 << 20;
        std::uint32_t max_header_bytes = 64 * 1024;
    };

    struct H2Stream;

    class H2Engine : public std::enable_shared_from_this<H2Engine> {
      public:
        ~H2Engine();
        H2Engine(const H2Engine &) = delete;
        H2Engine &operator=(const H2Engine &) = delete;

        [[nodiscard]] static std::shared_ptr<H2Engine> make_server(std::shared_ptr<H2Transport> transport, H2ServerConfig config, std::function<void()> on_closed);
        [[nodiscard]] static std::shared_ptr<H2Engine> make_client(std::shared_ptr<H2Transport> transport, H2ClientConfig config, std::function<void()> on_closed);

        /// Sends the connection preface/SETTINGS and starts reading. Call once.
        void start();

        /// Client: sends a request on a new stream. `request.target` is the path+query; `request.method`, headers and body
        /// are used as given (connection-specific headers are dropped). The handle fails with `ConnectionReset` if the
        /// connection can no longer take streams (GOAWAY / closed) -- the caller may retry on a fresh connection.
        [[nodiscard]] TaskHandle<std::expected<HttpResponseMessage, IoError>> submit(HttpRequestMessage request, std::string scheme, std::string authority,
                                                                                      CancellationToken cancellation);

        /// Client: false once the server sent GOAWAY or the connection died: open a new one for further requests.
        [[nodiscard]] bool usable() const noexcept { return usable_.load(std::memory_order_acquire); }

        void close();

      private:
        H2Engine(bool server, std::shared_ptr<H2Transport> transport);
        friend struct H2Callbacks;

        // io
        void pump_read();
        void pump_write();
        void fail_connection(const std::string &why);
        void drain_finished();

        // server side
        void request_complete(const std::shared_ptr<H2Stream> &stream);
        void submit_response(int32_t id, HttpResponseMessage response);

        // body source (responses on the server, request bodies on the client)
        nghttp2_ssize read_body(H2Stream &stream, uint8_t *buffer, size_t length, uint32_t *flags);
        void start_fetch(const std::shared_ptr<H2Stream> &stream);

        bool server_;
        std::shared_ptr<H2Transport> transport_;
        nghttp2_session *session_ = nullptr;
        H2ServerConfig server_config_;
        H2ClientConfig client_config_;
        std::function<void()> on_closed_;
        std::unordered_map<int32_t, std::shared_ptr<H2Stream>> streams_;
        std::vector<std::shared_ptr<H2Stream>> finished_; ///< client streams to complete once we are out of nghttp2 callbacks
        bool writing_ = false;
        bool reading_ = false;
        bool closed_ = false;
        bool started_ = false;
        std::atomic<bool> usable_{true};
    };

} // namespace SFT::Async::Detail

#endif
