#pragma once

// Streaming HTTP responses built from Async streams: the glue between `Stream<T>` and `HttpResponseMessage::body_stream`.
//
//   router.get("/events", [bus](const RouteRequest&) {
//       return sse_response(stream_of_channel(bus.subscribe()).map([](auto& m) { return SseEvent{.data = m.text, .event = "chat"}; }));
//   });

#include <Async/Http.hpp>
#include <Async/Stream.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace SFT::Async {

    /// One Server-Sent Event (WHATWG HTML spec, "text/event-stream").
    struct SseEvent {
        std::string data;                   ///< may contain newlines; each line becomes a `data:` field
        std::string event;                  ///< optional event name
        std::string id;                     ///< optional last-event-id
        std::optional<unsigned> retry_ms;   ///< optional reconnect delay hint
    };

    /// Wire format of an event, ending with the blank line that dispatches it. Fields containing a line break are
    /// split or dropped so a hostile value can never inject extra fields.
    [[nodiscard]] inline std::string format_sse(const SseEvent &event) {
        auto single_line = [](const std::string &text) {
            std::string out;
            for (char c : text)
                if (c != '\n' && c != '\r') out += c;
            return out;
        };
        std::string out;
        if (!event.event.empty()) out += "event: " + single_line(event.event) + "\n";
        if (!event.id.empty()) out += "id: " + single_line(event.id) + "\n";
        if (event.retry_ms) out += "retry: " + std::to_string(*event.retry_ms) + "\n";
        std::size_t start = 0;
        for (;;) {
            auto end = event.data.find('\n', start);
            std::string line = event.data.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out += "data: " + line + "\n";
            if (end == std::string::npos) break;
            start = end + 1;
        }
        out += "\n";
        return out;
    }

    /// A response whose body is the items of `stream`, sent as they appear (chunked transfer encoding). The server stops
    /// pulling when the client disconnects. Keep per-item work cheap or hand it to `par_map` upstream.
    [[nodiscard]] inline HttpResponseMessage stream_response(Stream<std::string> stream, std::string content_type = "application/octet-stream",
                                                             unsigned status = 200) {
        HttpResponseMessage response;
        response.status = status;
        response.headers.push_back({"Content-Type", std::move(content_type)});
        auto shared = std::make_shared<Stream<std::string>>(std::move(stream));
        response.body_stream = [shared](CancellationToken) { return shared->next(); };
        return response;
    }

    /// A Server-Sent Events response: one event per stream item, `Cache-Control: no-cache`, proxy buffering disabled.
    [[nodiscard]] inline HttpResponseMessage sse_response(Stream<SseEvent> events) {
        auto response = stream_response(std::move(events).map([](SseEvent event) { return format_sse(event); }), "text/event-stream");
        response.headers.push_back({"Cache-Control", "no-cache"});
        response.headers.push_back({"X-Accel-Buffering", "no"});
        return response;
    }

} // namespace SFT::Async
