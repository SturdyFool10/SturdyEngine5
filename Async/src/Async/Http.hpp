#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    struct HttpHeader {
        std::string name;
        std::string value;
    };

    struct HttpRequestMessage {
        std::string method;
        std::string target;
        std::uint32_t version = 11;
        std::vector<HttpHeader> headers;
        std::string body;
        /// Server side: the client's IP address (IPv4-mapped IPv6 addresses are normalised), e.g. for rate limiting.
        std::string remote_address;
        /// Server side, only when `HttpServerOptions::spool_requests_above` is set and the body was larger: the body lives
        /// in this temporary file (and `body` is empty). The file is deleted after the response is sent -- move it if you
        /// need to keep it.
        std::string body_file;
    };

    /// Produces a response body piece by piece: each call returns the next chunk (an empty optional ends the body).
    /// `cancel` fires if the client goes away or the server stops, so a producer waiting on an event source can stop.
    using HttpBodyStream = std::function<TaskHandle<std::optional<std::string>>(CancellationToken cancel)>;

    struct HttpResponseMessage {
        unsigned status = 200;
        std::vector<HttpHeader> headers;
        std::string body;
        bool keep_alive = true;
        /// Serve the body from this file instead of `body` (server side; streamed, never fully loaded).
        /// Combine with `body_offset`/`body_length` for byte ranges. Mutually exclusive with `body`.
        std::string body_file;
        std::uint64_t body_offset = 0;
        /// Bytes to send from `body_file` starting at `body_offset`; unset means "to the end of the file".
        std::optional<std::uint64_t> body_length;
        /// Server side: stream the body (Server-Sent Events, long downloads, generated data) with chunked transfer
        /// encoding instead of building it in memory. Mutually exclusive with `body` / `body_file`. See also
        /// `stream_response` / `sse_response` in <Async/HttpStreaming.hpp>.
        HttpBodyStream body_stream;
        /// Client side: the URL the response finally came from (after redirects).
        std::string final_url;
    };

    /// Case-insensitive header lookup; returns the first match.
    [[nodiscard]] inline std::optional<std::string_view> header_value(const std::vector<HttpHeader> &headers, std::string_view name) {
        for (const HttpHeader &header : headers) {
            if (header.name.size() == name.size() &&
                std::equal(name.begin(), name.end(), header.name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                return std::string_view(header.value);
            }
        }
        return std::nullopt;
    }

} // namespace SFT::Async
