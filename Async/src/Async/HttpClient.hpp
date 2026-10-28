#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/Http.hpp>
#include <Async/IoError.hpp>
#include <Async/Mutex.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {
    using Literals::operator""_KiB;
    using Literals::operator""_MiB;

    /// One stored cookie (RFC 6265).
    struct Cookie {
        std::string name;
        std::string value;
        std::string domain;            ///< lowercase, no leading dot
        std::string path = "/";
        bool host_only = true;         ///< no Domain attribute: only the exact host receives it
        bool secure = false;           ///< only sent over https
        bool http_only = false;
        /// Absent: a session cookie (kept until the jar is dropped).
        std::optional<std::chrono::system_clock::time_point> expires;
    };

    /// A thread-safe cookie store. `HttpClient` fills it from `Set-Cookie` headers (including on redirect hops) and sends
    /// matching cookies automatically. Defences: a Domain attribute must cover the request host and have at least two
    /// labels (no `Domain=com`), IP hosts only keep host-only cookies, cookies over 4 KiB are ignored, and the jar is
    /// capped (50 per domain, 3000 total, oldest evicted). Expired cookies are never sent.
    class CookieJar {
      public:
        void store_from_response(const std::string &host, const std::string &path, bool https, const std::vector<HttpHeader> &response_headers);
        /// "a=1; b=2" for the cookies that apply to this request, or empty.
        [[nodiscard]] std::string header_for(const std::string &host, const std::string &path, bool https) const;
        void add(Cookie cookie);
        [[nodiscard]] std::vector<Cookie> all() const;
        void clear();

      private:
        mutable Mutex<std::vector<Cookie>> cookies_;
    };

    struct HttpClientOptions {
        /// For https:// URLs: chain + host name verified by default (see `TlsClientOptions`).
        TlsClientOptions tls;
        /// Deadline for one `request` call, covering connecting, every redirect hop, and reading the response.
        std::chrono::milliseconds timeout{30'000};
        /// Redirects followed (301/302/303/307/308). 0 returns the redirect response itself.
        unsigned max_redirects = 5;
        /// An https:// request is never redirected to http:// unless this is set.
        bool allow_https_downgrade = false;
        /// Responses larger than this fail with `LimitExceeded` (the connection is dropped, memory stays bounded).
        std::uint64_t max_response_body_bytes = 16_MiB;
        std::uint32_t max_response_header_bytes = 64_KiB;
        /// Reuse connections between requests to the same host.
        bool keep_alive = true;
        std::size_t max_idle_connections_per_host = 4;
        std::chrono::milliseconds idle_connection_timeout{30'000};
        std::string user_agent = "SturdyEngine-Async";
        /// Offer HTTP/2 via ALPN on https:// connections. Requests to one origin then share a multiplexed connection when the
        /// server picks "h2"; otherwise (or without nghttp2 in the build) everything stays HTTP/1.1. Plain http:// is HTTP/1.1.
        bool http2 = true;
        /// Use HTTP/3 (QUIC) for https:// origins that advertise it with `Alt-Svc`, falling back to HTTP/2 / HTTP/1.1 when QUIC
        /// is blocked or fails (UDP is filtered on many networks; a failed origin is not retried for five minutes). Needs
        /// ngtcp2 + nghttp3 in the build. Not used through a proxy.
        bool http3 = true;
        /// Try HTTP/3 first even before an Alt-Svc was seen -- for servers you know speak it. Costs one failed attempt
        /// (bounded by `http3_connect_timeout`) against servers that do not.
        bool http3_prior_knowledge = false;
        std::chrono::milliseconds http3_connect_timeout{3'000};

        /// Ask for `gzip`/`deflate` and decode the body transparently (the limit above applies to the *decoded* size, so a
        /// compression bomb is cut off). The response then has no Content-Encoding header. Needs zlib in the build.
        bool decompress = true;
        /// Keep and send cookies. Pass your own jar to share it between clients or persist it; otherwise a private one is used.
        bool cookies = false;
        std::shared_ptr<CookieJar> cookie_jar;
        /// `http://[user:password@]host:port`. http:// targets are sent in absolute form; https:// targets go through a
        /// CONNECT tunnel and are TLS-verified end to end (the proxy sees only the host name). The proxy hop itself is
        /// plain HTTP.
        std::string proxy;
        /// Host names (or `.suffix` patterns) that bypass the proxy.
        std::vector<std::string> no_proxy;
    };

    namespace Detail {
        struct HttpClientCore;
    }

    /// An HTTP/1.1 client with connection pooling. Cheap to copy-share via `std::shared_ptr`; one instance per
    /// application is typical. All requests run on the network reactor; none occupies a worker thread.
    ///
    /// `Authorization`, `Cookie` and `Proxy-Authorization` headers are dropped when a redirect leaves the
    /// original scheme/host/port. Bodies are sent with Content-Length; responses are limited and never
    /// transparently decompressed (`Accept-Encoding: identity` is sent).
    class HttpClient {
      public:
        explicit HttpClient(HttpClientOptions options = {});
        ~HttpClient();
        HttpClient(HttpClient &&) noexcept;
        HttpClient &operator=(HttpClient &&) noexcept;
        HttpClient(const HttpClient &) = delete;
        HttpClient &operator=(const HttpClient &) = delete;

        /// Sends a request. Non-2xx statuses are ordinary successful results; only transport, TLS, protocol,
        /// limit and timeout problems are errors.
        [[nodiscard]] TaskHandle<std::expected<HttpResponseMessage, IoError>> request(
            std::string method, std::string url, std::vector<HttpHeader> headers = {}, std::string body = {},
            CancellationToken cancellation = {});

        [[nodiscard]] TaskHandle<std::expected<HttpResponseMessage, IoError>> get(std::string url, std::vector<HttpHeader> headers = {},
                                                                                   CancellationToken cancellation = {}) {
            return request("GET", std::move(url), std::move(headers), {}, std::move(cancellation));
        }
        [[nodiscard]] TaskHandle<std::expected<HttpResponseMessage, IoError>> post(std::string url, std::string body,
                                                                                    std::string content_type = "application/octet-stream",
                                                                                    std::vector<HttpHeader> headers = {},
                                                                                    CancellationToken cancellation = {}) {
            headers.push_back({"Content-Type", std::move(content_type)});
            return request("POST", std::move(url), std::move(headers), std::move(body), std::move(cancellation));
        }

        /// The jar in use (null unless `cookies` is on).
        [[nodiscard]] std::shared_ptr<CookieJar> cookie_jar() const;

        /// Closes idle pooled connections.
        void close_idle() noexcept;

      private:
        std::shared_ptr<Detail::HttpClientCore> core_;
    };

} // namespace SFT::Async
