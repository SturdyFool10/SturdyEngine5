#pragma once

// One byte-stream vocabulary for everything that moves bytes (tokio::io: AsyncRead / AsyncWrite / copy / BufReader /
// Framed). A type joins by specialising `IoTraits<T>`; TcpConnection, File, the child-process pipes and in-memory pipes
// already do. Algorithms are written once and work on all of them:
//
//   co_await copy(socket, file);                         // download to disk, read and write overlapped
//   BufferedReader reader(socket);  auto line = co_await reader.read_line(4096);
//   Framed<TcpConnection, LengthDelimitedCodec> wire(socket);  co_await wire.send(message);  auto next = co_await wire.next();
//
// The objects you pass must outlive the operations (the operations hold references, like any coroutine parameter).

#include <Async/Channel.hpp>
#include <Async/FileHandle.hpp>
#include <Async/Net.hpp>
#include <Async/Process.hpp>
#include <Async/Promise.hpp>
#include <Async/Stream.hpp>
#include <Async/Sync.hpp>
#include <Async/Task.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Async {

    namespace Io {
        using Bytes = std::vector<std::byte>;
        template <typename T>
        using Result = std::expected<T, IoError>;

        [[nodiscard]] inline Bytes to_bytes(std::string_view text) {
            Bytes out(text.size());
            if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
            return out;
        }
        [[nodiscard]] inline std::string to_string(const Bytes &bytes) { return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()); }
    } // namespace Io

    /// Specialise for your type: `read_some(T&, max, token) -> TaskHandle<Result<Bytes>>` (empty at end of stream),
    /// `write_all(T&, Bytes, token) -> TaskHandle<Result<void>>` and `shutdown(T&) -> TaskHandle<Result<void>>`
    /// (half-close / flush). Provide only the halves the type has.
    template <typename T>
    struct IoTraits;

    template <typename T>
    concept AsyncRead = requires(std::remove_cvref_t<T> &t, std::size_t n, CancellationToken token) {
        { IoTraits<std::remove_cvref_t<T>>::read_some(t, n, token) } -> std::same_as<TaskHandle<std::expected<std::vector<std::byte>, IoError>>>;
    };
    template <typename T>
    concept AsyncWrite = requires(std::remove_cvref_t<T> &t, std::vector<std::byte> data, CancellationToken token) {
        { IoTraits<std::remove_cvref_t<T>>::write_all(t, std::move(data), token) } -> std::same_as<TaskHandle<std::expected<void, IoError>>>;
    };

    // ---- built-in adaptations -------------------------------------------------------------------------------------
    template <>
    struct IoTraits<TcpConnection> {
        static auto read_some(TcpConnection &c, std::size_t n, CancellationToken t) { return c.receive(n, std::move(t)); }
        static auto write_all(TcpConnection &c, Io::Bytes d, CancellationToken t) { return c.send_all(std::move(d), std::move(t)); }
        static auto shutdown(TcpConnection &c) { return c.shutdown_write(); }
    };

    template <>
    struct IoTraits<File> {
        static auto read_some(File &f, std::size_t n, CancellationToken t) { return f.read(n, std::move(t)); }
        static TaskHandle<Io::Result<void>> write_all(File &f, Io::Bytes d, CancellationToken t) {
            return Detail::map_inline(f.write(std::move(d), std::move(t)), [](Io::Result<std::size_t> r) -> Io::Result<void> {
                return r ? Io::Result<void>() : Io::Result<void>(std::unexpected(std::move(r.error())));
            });
        }
        static auto shutdown(File &f) { return Detail::map_inline(f.sync(), [](Io::Result<void> r) { return r; }); }
    };

    /// The child's stdout as a reader (`Process` itself has three streams, so pick one).
    struct ProcessStdout {
        Process &process;
    };
    struct ProcessStderr {
        Process &process;
    };
    /// The child's stdin as a writer; `shutdown` closes it.
    struct ProcessStdin {
        Process &process;
    };
    template <>
    struct IoTraits<ProcessStdout> {
        static auto read_some(ProcessStdout &s, std::size_t n, CancellationToken t) { return s.process.read_stdout(n, std::move(t)); }
    };
    template <>
    struct IoTraits<ProcessStderr> {
        static auto read_some(ProcessStderr &s, std::size_t n, CancellationToken t) { return s.process.read_stderr(n, std::move(t)); }
    };
    template <>
    struct IoTraits<ProcessStdin> {
        static auto write_all(ProcessStdin &s, Io::Bytes d, CancellationToken t) { return s.process.write_stdin(std::move(d), std::move(t)); }
        static TaskHandle<Io::Result<void>> shutdown(ProcessStdin &s) {
            s.process.close_stdin();
            return ready(Io::Result<void>());
        }
    };

    /// A reader over bytes already in memory.
    class MemoryReader {
      public:
        explicit MemoryReader(Io::Bytes data = {}) : data_(std::move(data)) {}
        explicit MemoryReader(std::string_view text) : data_(Io::to_bytes(text)) {}
        [[nodiscard]] Io::Result<Io::Bytes> take(std::size_t max) {
            const std::size_t n = (std::min)(max, data_.size() - position_);
            Io::Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(position_), data_.begin() + static_cast<std::ptrdiff_t>(position_ + n));
            position_ += n;
            return out;
        }

      private:
        Io::Bytes data_;
        std::size_t position_ = 0;
    };
    template <>
    struct IoTraits<MemoryReader> {
        static TaskHandle<Io::Result<Io::Bytes>> read_some(MemoryReader &r, std::size_t n, CancellationToken) { return ready(r.take(n)); }
    };

    /// A writer that appends to a vector.
    class MemoryWriter {
      public:
        [[nodiscard]] const Io::Bytes &bytes() const noexcept { return data_; }
        [[nodiscard]] std::string text() const { return Io::to_string(data_); }
        void append(const Io::Bytes &more) { data_.insert(data_.end(), more.begin(), more.end()); }

      private:
        Io::Bytes data_;
    };
    template <>
    struct IoTraits<MemoryWriter> {
        static TaskHandle<Io::Result<void>> write_all(MemoryWriter &w, Io::Bytes d, CancellationToken) {
            w.append(d);
            return ready(Io::Result<void>());
        }
        static TaskHandle<Io::Result<void>> shutdown(MemoryWriter &) { return ready(Io::Result<void>()); }
    };

    /// One end of an in-process duplex byte pipe with back-pressure (a bounded channel of chunks each way).
    class PipeEnd {
      public:
        PipeEnd(Sender<Io::Bytes> tx, Receiver<Io::Bytes> rx) : tx_(std::move(tx)), rx_(std::move(rx)) {}
        [[nodiscard]] Sender<Io::Bytes> &sender() { return tx_; }
        [[nodiscard]] Receiver<Io::Bytes> &receiver() { return rx_; }
        Io::Bytes leftover;

      private:
        Sender<Io::Bytes> tx_;
        Receiver<Io::Bytes> rx_;
    };
    /// Two connected ends: bytes written to one are read from the other. `chunks` bounds how many writes may be queued.
    [[nodiscard]] inline std::pair<std::shared_ptr<PipeEnd>, std::shared_ptr<PipeEnd>> memory_pipe(std::size_t chunks = 8) {
        auto [a_tx, b_rx] = channel<Io::Bytes>(chunks);
        auto [b_tx, a_rx] = channel<Io::Bytes>(chunks);
        return {std::make_shared<PipeEnd>(std::move(a_tx), std::move(a_rx)), std::make_shared<PipeEnd>(std::move(b_tx), std::move(b_rx))};
    }
    template <>
    struct IoTraits<PipeEnd> {
        static TaskHandle<Io::Result<Io::Bytes>> read_some(PipeEnd &end, std::size_t n, CancellationToken token) {
            if (!end.leftover.empty()) {
                const std::size_t take = (std::min)(n, end.leftover.size());
                Io::Bytes out(end.leftover.begin(), end.leftover.begin() + static_cast<std::ptrdiff_t>(take));
                end.leftover.erase(end.leftover.begin(), end.leftover.begin() + static_cast<std::ptrdiff_t>(take));
                return ready(Io::Result<Io::Bytes>(std::move(out)));
            }
            return Detail::map_inline(end.receiver().recv(std::move(token)), [&end, n](std::optional<Io::Bytes> chunk) -> Io::Result<Io::Bytes> {
                if (!chunk) return Io::Bytes{};
                if (chunk->size() > n) {
                    end.leftover.assign(chunk->begin() + static_cast<std::ptrdiff_t>(n), chunk->end());
                    chunk->resize(n);
                }
                return std::move(*chunk);
            });
        }
        static TaskHandle<Io::Result<void>> write_all(PipeEnd &end, Io::Bytes d, CancellationToken) {
            Promise<Io::Result<void>> promise;
            auto sent = end.sender().send(std::move(d));
            sent.on_done([sent, promise]() mutable {
                try {
                    sent.wait();
                    (void)promise.set_value(Io::Result<void>());
                } catch (...) {
                    (void)promise.set_value(Io::Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "the other end of the pipe is closed"})));
                }
            });
            return promise.handle();
        }
        static TaskHandle<Io::Result<void>> shutdown(PipeEnd &end) {
            end.sender().close();
            return ready(Io::Result<void>());
        }
    };
    template <typename T>
        requires requires { typename IoTraits<T>; }
    struct IoTraits<std::shared_ptr<T>> {
        static auto read_some(std::shared_ptr<T> &p, std::size_t n, CancellationToken t) requires AsyncRead<T> { return IoTraits<T>::read_some(*p, n, std::move(t)); }
        static auto write_all(std::shared_ptr<T> &p, Io::Bytes d, CancellationToken t) requires AsyncWrite<T> { return IoTraits<T>::write_all(*p, std::move(d), std::move(t)); }
        static auto shutdown(std::shared_ptr<T> &p) { return IoTraits<T>::shutdown(*p); }
    };

    // ---- algorithms ----------------------------------------------------------------------------------------------
    struct CopyOptions {
        std::size_t chunk_bytes = 64 * 1024;
        /// Stop after this many bytes (the reader may have more).
        std::uint64_t limit = static_cast<std::uint64_t>(-1);
        /// Call `shutdown` on the writer when the reader reaches end of stream (half-closes a socket, closes a child's stdin).
        bool shutdown_writer = false;
    };

    /// Copies everything from `reader` to `writer` and resolves to the byte count. Reading the next chunk overlaps with
    /// writing the previous one, so a slow writer is the only thing that slows it down (and then reading pauses: no
    /// unbounded buffering). Fails with the first read or write error.
    template <AsyncRead R, AsyncWrite W>
    [[nodiscard]] TaskHandle<std::expected<std::uint64_t, IoError>> copy(R &reader, W &writer, CopyOptions options = {}, CancellationToken cancellation = {}) {
        using Reader = IoTraits<std::remove_cvref_t<R>>;
        using Writer = IoTraits<std::remove_cvref_t<W>>;
        std::uint64_t total = 0;
        auto want = [&]() -> std::size_t { return static_cast<std::size_t>((std::min<std::uint64_t>)(options.chunk_bytes, options.limit - total)); };
        if (options.limit == 0) co_return std::expected<std::uint64_t, IoError>(0);
        auto next = Reader::read_some(reader, want(), cancellation);
        for (;;) {
            auto chunk = co_await next;
            if (!chunk) co_return std::unexpected(chunk.error());
            if (chunk->empty()) break;
            total += chunk->size();
            auto written = Writer::write_all(writer, std::move(*chunk), cancellation);
            const bool more = total < options.limit;
            if (more) next = Reader::read_some(reader, want(), cancellation); // overlap the next read with this write
            auto result = co_await written;
            if (!result) co_return std::unexpected(result.error());
            if (!more) break;
        }
        if (options.shutdown_writer) {
            if constexpr (requires { Writer::shutdown(writer); }) {
                auto closed = co_await Writer::shutdown(writer);
                if (!closed) co_return std::unexpected(closed.error());
            }
        }
        co_return std::expected<std::uint64_t, IoError>(total);
    }

    /// Reads to end of stream, failing with `LimitExceeded` past `max_bytes`.
    template <AsyncRead R>
    [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_to_end(R &reader, std::size_t max_bytes = static_cast<std::size_t>(-1),
                                                                                         CancellationToken cancellation = {}) {
        using Reader = IoTraits<std::remove_cvref_t<R>>;
        std::vector<std::byte> out;
        for (;;) {
            auto chunk = co_await Reader::read_some(reader, 64 * 1024, cancellation);
            if (!chunk) co_return std::unexpected(chunk.error());
            if (chunk->empty()) break;
            if (out.size() + chunk->size() > max_bytes) co_return std::unexpected(IoError{IoErrorCode::LimitExceeded, "stream is longer than the allowed maximum"});
            out.insert(out.end(), chunk->begin(), chunk->end());
        }
        co_return std::expected<std::vector<std::byte>, IoError>(std::move(out));
    }

    /// Writes all of `data` then, optionally, half-closes.
    template <AsyncWrite W>
    [[nodiscard]] TaskHandle<std::expected<void, IoError>> write_all(W &writer, std::vector<std::byte> data, CancellationToken cancellation = {}) {
        return IoTraits<std::remove_cvref_t<W>>::write_all(writer, std::move(data), std::move(cancellation));
    }

    /// A reader with its own buffer: lines, delimiters and exact counts on top of any `AsyncRead`.
    template <AsyncRead R>
    class BufferedReader {
        using Traits = IoTraits<std::remove_cvref_t<R>>;

      public:
        explicit BufferedReader(R &inner, std::size_t chunk_bytes = 16 * 1024) : inner_(&inner), chunk_(chunk_bytes) {}

        /// Up to `max_bytes`, from the buffer first; empty at end of stream.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_some(std::size_t max_bytes, CancellationToken cancellation = {}) {
            return read_some_impl(this, max_bytes, std::move(cancellation));
        }
        /// Exactly `count` bytes, or `UnexpectedEof`.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_exact(std::size_t count, CancellationToken cancellation = {}) {
            return read_exact_impl(this, count, std::move(cancellation));
        }
        /// Bytes up to and including `delimiter`; `LimitExceeded` past `max_bytes` (the stream is then positioned after what
        /// was read, so treat the connection as poisoned). At a clean end of stream with nothing buffered: empty optional.
        [[nodiscard]] TaskHandle<std::expected<std::optional<std::vector<std::byte>>, IoError>> read_until(std::string delimiter, std::size_t max_bytes,
                                                                                                           CancellationToken cancellation = {}) {
            return read_until_impl(this, std::move(delimiter), max_bytes, std::move(cancellation));
        }
        /// A line without its terminator (\n or \r\n), or an empty optional at end of stream.
        [[nodiscard]] TaskHandle<std::expected<std::optional<std::string>, IoError>> read_line(std::size_t max_bytes = 64 * 1024, CancellationToken cancellation = {}) {
            return read_line_impl(this, max_bytes, std::move(cancellation));
        }
        [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - position_; }

      private:
        static TaskHandle<std::expected<bool, IoError>> fill(BufferedReader *self, CancellationToken cancellation) {
            auto chunk = co_await Traits::read_some(*self->inner_, self->chunk_, std::move(cancellation));
            if (!chunk) co_return std::unexpected(chunk.error());
            if (chunk->empty()) co_return std::expected<bool, IoError>(false);
            if (self->position_ == self->buffer_.size()) {
                self->buffer_.clear();
                self->position_ = 0;
            }
            self->buffer_.insert(self->buffer_.end(), chunk->begin(), chunk->end());
            co_return std::expected<bool, IoError>(true);
        }

        std::vector<std::byte> take(std::size_t count) {
            std::vector<std::byte> out(buffer_.begin() + static_cast<std::ptrdiff_t>(position_), buffer_.begin() + static_cast<std::ptrdiff_t>(position_ + count));
            position_ += count;
            return out;
        }

        static TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_some_impl(BufferedReader *self, std::size_t max_bytes, CancellationToken cancellation) {
            if (self->buffered() == 0) {
                auto filled = co_await fill(self, std::move(cancellation));
                if (!filled) co_return std::unexpected(filled.error());
                if (!*filled) co_return std::expected<std::vector<std::byte>, IoError>(std::vector<std::byte>{});
            }
            co_return std::expected<std::vector<std::byte>, IoError>(self->take((std::min)(max_bytes, self->buffered())));
        }

        static TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_exact_impl(BufferedReader *self, std::size_t count, CancellationToken cancellation) {
            while (self->buffered() < count) {
                auto filled = co_await fill(self, cancellation);
                if (!filled) co_return std::unexpected(filled.error());
                if (!*filled) co_return std::unexpected(IoError{IoErrorCode::UnexpectedEof, "stream ended before the requested bytes arrived"});
            }
            co_return std::expected<std::vector<std::byte>, IoError>(self->take(count));
        }

        static TaskHandle<std::expected<std::optional<std::vector<std::byte>>, IoError>> read_until_impl(BufferedReader *self, std::string delimiter,
                                                                                                         std::size_t max_bytes, CancellationToken cancellation) {
            using Out = std::expected<std::optional<std::vector<std::byte>>, IoError>;
            std::size_t scanned = 0;
            for (;;) {
                const auto *begin = reinterpret_cast<const char *>(self->buffer_.data()) + self->position_;
                const std::string_view view(begin, self->buffered());
                const std::size_t from = scanned >= delimiter.size() ? scanned - delimiter.size() + 1 : 0;
                if (const auto at = view.find(delimiter, from); at != std::string_view::npos) {
                    const std::size_t length = at + delimiter.size();
                    if (length > max_bytes) co_return std::unexpected(IoError{IoErrorCode::LimitExceeded, "delimiter found beyond the byte limit"});
                    co_return Out(std::optional<std::vector<std::byte>>(self->take(length)));
                }
                scanned = view.size();
                if (scanned >= max_bytes) co_return std::unexpected(IoError{IoErrorCode::LimitExceeded, "delimiter not found within the byte limit"});
                auto filled = co_await fill(self, cancellation);
                if (!filled) co_return std::unexpected(filled.error());
                if (!*filled) {
                    if (self->buffered() == 0) co_return Out(std::optional<std::vector<std::byte>>{});
                    co_return std::unexpected(IoError{IoErrorCode::UnexpectedEof, "stream ended in the middle of a record"});
                }
            }
        }

        static TaskHandle<std::expected<std::optional<std::string>, IoError>> read_line_impl(BufferedReader *self, std::size_t max_bytes, CancellationToken cancellation) {
            using Out = std::expected<std::optional<std::string>, IoError>;
            auto record = co_await read_until_impl(self, "\n", max_bytes, std::move(cancellation));
            if (!record) co_return std::unexpected(record.error());
            if (!*record) co_return Out(std::optional<std::string>{});
            std::string line = Io::to_string(**record);
            line.pop_back();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            co_return Out(std::optional<std::string>(std::move(line)));
        }

        R *inner_;
        std::size_t chunk_;
        std::vector<std::byte> buffer_;
        std::size_t position_ = 0;
    };

    /// A writer that batches small writes into `capacity`-sized sends. Call `flush()` (or `shutdown()`) before relying on delivery.
    template <AsyncWrite W>
    class BufferedWriter {
        using Traits = IoTraits<std::remove_cvref_t<W>>;

      public:
        explicit BufferedWriter(W &inner, std::size_t capacity = 16 * 1024) : inner_(&inner), capacity_(capacity) {}

        /// Buffers `data`, sending a full buffer whenever one accumulates. Await it before the next call.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> write(std::vector<std::byte> data, CancellationToken cancellation = {}) {
            return write_impl(this, std::move(data), std::move(cancellation));
        }
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> flush(CancellationToken cancellation = {}) { return flush_impl(this, std::move(cancellation)); }
        /// Flush, then half-close the underlying writer (if it supports it).
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> shutdown(CancellationToken cancellation = {}) { return shutdown_impl(this, std::move(cancellation)); }

      private:
        static TaskHandle<std::expected<void, IoError>> flush_impl(BufferedWriter *self, CancellationToken cancellation) {
            if (!self->pending_.empty()) {
                auto result = co_await Traits::write_all(*self->inner_, std::exchange(self->pending_, {}), std::move(cancellation));
                if (!result) co_return result;
            }
            co_return std::expected<void, IoError>();
        }
        static TaskHandle<std::expected<void, IoError>> write_impl(BufferedWriter *self, std::vector<std::byte> data, CancellationToken cancellation) {
            if (data.size() >= self->capacity_) { // too big to batch: preserve order, then send it straight through
                auto flushed = co_await flush_impl(self, cancellation);
                if (!flushed) co_return flushed;
                co_return co_await Traits::write_all(*self->inner_, std::move(data), std::move(cancellation));
            }
            if (self->pending_.size() + data.size() > self->capacity_) {
                auto flushed = co_await flush_impl(self, cancellation);
                if (!flushed) co_return flushed;
            }
            self->pending_.insert(self->pending_.end(), data.begin(), data.end());
            co_return std::expected<void, IoError>();
        }
        static TaskHandle<std::expected<void, IoError>> shutdown_impl(BufferedWriter *self, CancellationToken cancellation) {
            auto flushed = co_await flush_impl(self, std::move(cancellation));
            if (!flushed) co_return flushed;
            if constexpr (requires { Traits::shutdown(*self->inner_); }) co_return co_await Traits::shutdown(*self->inner_);
            co_return std::expected<void, IoError>();
        }

        W *inner_;
        std::size_t capacity_;
        std::vector<std::byte> pending_;
    };

    // ---- framing -----------------------------------------------------------------------------------------------
    /// A codec turns a byte stream into frames: `decode(buffer)` consumes one complete frame from the front of the
    /// buffer (returning it), or returns an empty optional if more bytes are needed, or an error for corrupt data;
    /// `encode(frame, out)` appends a frame's wire form.

    /// `u32` big-endian length prefix, then that many bytes. Frames above `max_frame_bytes` are rejected on both sides, so a
    /// hostile length can never make the receiver allocate gigabytes.
    struct LengthDelimitedCodec {
        using frame_type = std::vector<std::byte>;
        std::size_t max_frame_bytes = 8 * 1024 * 1024;

        [[nodiscard]] Io::Result<std::optional<frame_type>> decode(std::vector<std::byte> &buffer) const {
            if (buffer.size() < 4) return std::optional<frame_type>{};
            const std::uint32_t length = (std::to_integer<std::uint32_t>(buffer[0]) << 24) | (std::to_integer<std::uint32_t>(buffer[1]) << 16) |
                                         (std::to_integer<std::uint32_t>(buffer[2]) << 8) | std::to_integer<std::uint32_t>(buffer[3]);
            if (length > max_frame_bytes) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "frame length exceeds the codec limit"});
            if (buffer.size() < 4 + static_cast<std::size_t>(length)) return std::optional<frame_type>{};
            frame_type frame(buffer.begin() + 4, buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
            buffer.erase(buffer.begin(), buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
            return std::optional<frame_type>(std::move(frame));
        }
        [[nodiscard]] Io::Result<void> encode(const frame_type &frame, std::vector<std::byte> &out) const {
            if (frame.size() > max_frame_bytes) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "frame exceeds the codec limit"});
            const auto length = static_cast<std::uint32_t>(frame.size());
            out.push_back(static_cast<std::byte>(length >> 24));
            out.push_back(static_cast<std::byte>(length >> 16));
            out.push_back(static_cast<std::byte>(length >> 8));
            out.push_back(static_cast<std::byte>(length));
            out.insert(out.end(), frame.begin(), frame.end());
            return {};
        }
    };

    /// Text lines ending in \n (a trailing \r is dropped). Lines longer than `max_line_bytes` are an error.
    struct LinesCodec {
        using frame_type = std::string;
        std::size_t max_line_bytes = 64 * 1024;

        [[nodiscard]] Io::Result<std::optional<frame_type>> decode(std::vector<std::byte> &buffer) const {
            const auto *begin = reinterpret_cast<const char *>(buffer.data());
            const std::string_view view(begin, buffer.size());
            const auto at = view.find('\n');
            if (at == std::string_view::npos) {
                if (buffer.size() > max_line_bytes) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "line exceeds the codec limit"});
                return std::optional<frame_type>{};
            }
            if (at > max_line_bytes) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "line exceeds the codec limit"});
            std::string line(view.substr(0, at));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(at) + 1);
            return std::optional<frame_type>(std::move(line));
        }
        [[nodiscard]] Io::Result<void> encode(const frame_type &frame, std::vector<std::byte> &out) const {
            if (frame.size() > max_line_bytes || frame.find('\n') != std::string::npos) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "a line must be short and contain no newline"});
            const auto bytes = Io::to_bytes(frame);
            out.insert(out.end(), bytes.begin(), bytes.end());
            out.push_back(std::byte{'\n'});
            return {};
        }
    };

    template <typename C>
    concept FrameCodec = requires(const C &c, std::vector<std::byte> &buffer, const typename C::frame_type &frame) {
        { c.decode(buffer) } -> std::same_as<std::expected<std::optional<typename C::frame_type>, IoError>>;
        { c.encode(frame, buffer) } -> std::same_as<std::expected<void, IoError>>;
    };

    /// Frames over any bidirectional stream: `next()` yields decoded frames, `send(frame)` encodes and writes one.
    template <typename Io_, FrameCodec Codec>
        requires AsyncRead<Io_> && AsyncWrite<Io_>
    class Framed {
        using Traits = IoTraits<std::remove_cvref_t<Io_>>;

      public:
        using Frame = typename Codec::frame_type;
        Framed(Io_ &stream, Codec codec = {}) : stream_(&stream), codec_(std::move(codec)) {}

        /// The next complete frame; an empty optional at a clean end of stream; `UnexpectedEof` if the peer stopped in the
        /// middle of a frame; the codec's error for corrupt data.
        [[nodiscard]] TaskHandle<std::expected<std::optional<Frame>, IoError>> next(CancellationToken cancellation = {}) { return next_impl(this, std::move(cancellation)); }

        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send(const Frame &frame, CancellationToken cancellation = {}) {
            std::vector<std::byte> wire;
            if (auto encoded = codec_.encode(frame, wire); !encoded) return ready(std::expected<void, IoError>(std::unexpected(encoded.error())));
            return Traits::write_all(*stream_, std::move(wire), std::move(cancellation));
        }

        /// The incoming frames as a Stream (a codec or transport error ends it with that error).
        [[nodiscard]] Stream<Frame> frames() {
            return stream_from([this]() -> TaskHandle<std::optional<Frame>> {
                Promise<std::optional<Frame>> promise;
                auto pending = next();
                pending.on_done([pending, promise]() mutable {
                    try {
                        auto result = pending.wait();
                        if (!result) (void)promise.set_exception(std::make_exception_ptr(std::runtime_error(result.error().message)));
                        else (void)promise.set_value(std::move(*result));
                    } catch (...) {
                        (void)promise.set_exception(std::current_exception());
                    }
                });
                return promise.handle();
            });
        }

      private:
        static TaskHandle<std::expected<std::optional<Frame>, IoError>> next_impl(Framed *self, CancellationToken cancellation) {
            using Out = std::expected<std::optional<Frame>, IoError>;
            for (;;) {
                auto decoded = self->codec_.decode(self->buffer_);
                if (!decoded) co_return std::unexpected(decoded.error());
                if (*decoded) co_return Out(std::move(*decoded));
                auto chunk = co_await Traits::read_some(*self->stream_, 16 * 1024, cancellation);
                if (!chunk) co_return std::unexpected(chunk.error());
                if (chunk->empty()) {
                    if (self->buffer_.empty()) co_return Out(std::optional<Frame>{});
                    co_return std::unexpected(IoError{IoErrorCode::UnexpectedEof, "stream ended in the middle of a frame"});
                }
                self->buffer_.insert(self->buffer_.end(), chunk->begin(), chunk->end());
            }
        }

        Io_ *stream_;
        Codec codec_;
        std::vector<std::byte> buffer_;
    };

} // namespace SFT::Async
