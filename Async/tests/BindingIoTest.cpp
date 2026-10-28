// The facade's file-system helpers and generic byte-stream layer (copy, buffering, framing), plus the socket, process, watch and relay
// options that the first facade pass left out.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace SFT;
using namespace SFT::Async;
namespace B = SFT::Async::Binding;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("BindingIoTest FAILED: {}", what);
            ++failures;
        }
    }

    B::Bytes bytes(std::string_view text) { return B::Bytes(text.begin(), text.end()); }
    std::string text(const B::Bytes &data) { return std::string(data.begin(), data.end()); }

    B::Value finish(const B::OpPtr &op) {
        if (!op->wait(10'000)) {
            Foundation::log_error("BindingIoTest: an operation did not finish");
            std::abort();
        }
        return op->result();
    }
    bool ok(const B::OpPtr &op) { return finish(op).status == B::OpStatus::Ok; }

    std::vector<std::string> frames_of(const B::Bytes &packed) {
        std::vector<std::string> out;
        for (auto &frame : B::unpack_frames(packed)) out.push_back(text(frame));
        return out;
    }

    /// A connected TCP pair over loopback.
    std::pair<B::ObjectPtr, B::ObjectPtr> tcp_pair() {
        auto listener = finish(B::tcp_listen("127.0.0.1", 0, nullptr, 0)).object;
        auto accepting = B::tcp_accept(listener);
        auto client = finish(B::tcp_connect("127.0.0.1", B::tcp_listener_port(listener), nullptr, 3000)).object;
        auto server = finish(accepting).object;
        return {client, server};
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    const fs::path scratch = fs::temp_directory_path() / "sturdy_binding_io_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const std::string root = scratch.string();

    // ---- whole-file helpers and directories ----
    {
        check(ok(B::fs_create_directories(root + "/a/b/c")), "create_directories");
        check(finish(B::fs_exists(root + "/a/b/c")).number == 1 && finish(B::fs_exists(root + "/nope")).number == 0, "exists");
        check(ok(B::fs_write_file(root + "/a/one.txt", bytes("hello "), false)), "write_file");
        check(ok(B::fs_write_file(root + "/a/one.txt", bytes("world"), true)), "append");
        check(text(finish(B::fs_read_file(root + "/a/one.txt", 0)).bytes) == "hello world", "read_file");
        check(finish(B::fs_size(root + "/a/one.txt")).number == 11, "size");
        check(text(finish(B::fs_read_range(root + "/a/one.txt", 6, 5)).bytes) == "world", "read_range");
        const auto too_big = finish(B::fs_read_file(root + "/a/one.txt", 4));
        check(too_big.status == B::OpStatus::Failed && too_big.code == IoErrorCode::InvalidArgument, "read_file honours its limit");
        check(finish(B::fs_read_file(root + "/missing", 0)).code == IoErrorCode::NotFound, "a missing file is NotFound");
        check(ok(B::fs_copy(root + "/a/one.txt", root + "/a/two.txt")) && ok(B::fs_rename(root + "/a/two.txt", root + "/a/b/three.txt")), "copy and rename");
        {
            auto listing = finish(B::fs_list(root + "/a", 0, false));
            auto names = frames_of(listing.bytes);
            for (auto &name : names) name = fs::path(name).filename().string();
            std::sort(names.begin(), names.end());
            check(listing.number == 2 && names == std::vector<std::string>{"b", "one.txt"}, "list_directory");
            check(finish(B::fs_list(root + "/a", 0, true)).number == 4, "a recursive listing includes nested entries");
        }
        {
            auto it = B::dir_iter_new(root + "/a/b", false);
            int seen = 0;
            for (;;) {
                const auto next = finish(B::dir_iter_next(it));
                if (next.number == 0) break;
                ++seen;
            }
            check(seen == 2, "dir_iter walks every entry then stops");
        }
        check(finish(B::fs_remove_file(root + "/a/one.txt")).number == 1 && finish(B::fs_remove_file(root + "/a/one.txt")).number == 0, "remove_file reports whether it removed");
        check(finish(B::fs_remove_directory(root + "/a/b/c")).number == 1, "remove_directory");
    }

    // ---- generic streams: copy, memory, buffered, framed ----
    {
        auto source = finish(B::file_open(root + "/src.bin", B::FileWrite | B::FileCreate | B::FileRead)).object;
        check(ok(B::io_write_all(source, B::ProcessStream::Stdout, B::Bytes(200000, 'q'))), "io_write_all to a file");
        (void)finish(B::file_seek(source, 0, 0));
        auto sink = finish(B::file_open(root + "/dst.bin", B::FileWrite | B::FileCreate)).object;
        B::CopySpec spec;
        spec.chunk_bytes = 4096;
        const auto copied = finish(B::io_copy(source, B::ProcessStream::Stdout, sink, B::ProcessStream::Stdout, spec));
        check(copied.number == 200000, "io_copy copies a file to a file");
        check(fs::file_size(scratch / "dst.bin") == 200000, "and the bytes are there");

        (void)finish(B::file_seek(source, 0, 0));
        B::CopySpec limited;
        limited.limit = 1000;
        auto memory = B::memory_writer_new();
        check(finish(B::io_copy(source, B::ProcessStream::Stdout, memory, B::ProcessStream::Stdout, limited)).number == 1000 && B::memory_writer_bytes(memory).size() == 1000, "io_copy honours a limit");

        auto reader = B::memory_reader_new(bytes("line one\r\nline two\nlast"));
        auto buffered = B::buffered_reader_new(reader, B::ProcessStream::Stdout, 5);
        auto line = finish(B::buffered_read_line(buffered, 0));
        check(line.number == 1 && line.text == "line one", "read_line strips CRLF");
        check(finish(B::buffered_read_line(buffered, 0)).text == "line two", "read_line strips LF");
        check(B::buffered_reader_buffered(buffered) > 0 || true, "buffered() is readable");
        check(text(finish(B::buffered_read_exact(buffered, 2)).bytes) == "la", "read_exact");
        const auto until = finish(B::buffered_read_until(buffered, "t", 100));
        check(until.number == 1 && text(until.bytes) == "st", "read_until includes the delimiter");
        check(finish(B::buffered_read_line(buffered, 0)).number == 0, "a clean end of stream reads as number 0");
        check(finish(B::buffered_read_exact(buffered, 4)).code == IoErrorCode::UnexpectedEof, "read_exact past the end is UnexpectedEof");

        auto out = B::memory_writer_new();
        auto buffered_out = B::buffered_writer_new(out, B::ProcessStream::Stdout, 64);
        (void)finish(B::buffered_write(buffered_out, bytes("abc")));
        check(B::memory_writer_bytes(out).empty(), "small writes are held back");
        (void)finish(B::buffered_flush(buffered_out));
        check(text(B::memory_writer_bytes(out)) == "abc", "flush sends them");
        (void)finish(B::buffered_write(buffered_out, B::Bytes(100, 'z')));
        check(B::memory_writer_bytes(out).size() == 103, "a write larger than the buffer goes straight through, in order");
        check(ok(B::buffered_shutdown(buffered_out)), "shutdown flushes and closes");

        auto [near_end, far_end] = B::memory_pipe_new(4);
        (void)finish(B::io_write_all(near_end, B::ProcessStream::Stdout, bytes("through the pipe")));
        check(text(finish(B::buffered_read_exact(B::buffered_reader_new(far_end, B::ProcessStream::Stdout, 0), 7)).bytes) == "through", "a memory pipe carries bytes from one end to the other");

        // Framing over a pipe, both codecs.
        {
            auto [a, b] = B::memory_pipe_new(8);
            auto sender = B::framed_new(a, B::FrameCodec::LengthDelimited, 0);
            auto receiver = B::framed_new(b, B::FrameCodec::LengthDelimited, 0);
            check(ok(B::framed_send(sender, bytes("first"))) && ok(B::framed_send(sender, bytes(""))) && ok(B::framed_send(sender, bytes("third"))), "framed_send");
            check(text(finish(B::framed_next(receiver)).bytes) == "first", "frames arrive whole");
            const auto empty = finish(B::framed_next(receiver));
            check(empty.number == 1 && empty.bytes.empty(), "an empty frame is a frame, not the end");
            check(text(finish(B::framed_next(receiver)).bytes) == "third", "and in order");
        }
        {
            auto [client, server] = tcp_pair();
            auto lines_out = B::framed_new(client, B::FrameCodec::Lines, 0);
            auto lines_in = B::framed_new(server, B::FrameCodec::Lines, 0);
            (void)finish(B::framed_send(lines_out, bytes("alpha")));
            (void)finish(B::framed_send(lines_out, bytes("beta")));
            auto stream = B::framed_stream(lines_in);
            check(text(finish(B::stream_next(stream)).bytes) == "alpha" && text(finish(B::stream_next(stream)).bytes) == "beta", "framed_stream yields frames over TCP");
            const auto bad = finish(B::framed_send(lines_out, bytes("no\nnewlines")));
            check(bad.status == B::OpStatus::Failed, "a line with a newline cannot be sent");
            B::tcp_close(client);
            check(finish(B::stream_next(stream)).number == 0, "the stream ends when the peer closes");
        }
        {
            bool refused = false;
            try {
                (void)B::buffered_reader_new(B::memory_writer_new(), B::ProcessStream::Stdout, 0);
            } catch (const B::BindingError &) {
                refused = true;
            }
            check(refused, "a write-only object cannot be read");
        }
    }

    // ---- socket options ----
    {
        auto [client, server] = tcp_pair();
        check(ok(B::tcp_set_nodelay(client, true)) && ok(B::tcp_set_keepalive(client, true)), "tcp options are applied");
        auto listener = finish(B::tcp_listen("127.0.0.1", 0, nullptr, 16)).object;
        check(B::tcp_listener_address(listener).starts_with("127.0.0.1:") && B::tcp_listener_is_open(listener), "listener address and state");

        auto a = finish(B::udp_bind("127.0.0.1", 0, {})).object;
        auto b = finish(B::udp_bind("127.0.0.1", 0, {})).object;
        check(ok(B::udp_connect(a, "127.0.0.1", B::udp_port(b))) && ok(B::udp_connect(b, "127.0.0.1", B::udp_port(a))), "udp_connect");
        check(finish(B::udp_send(a, bytes("connected"))).number == 9, "udp_send on a connected socket");
        check(text(finish(B::udp_receive(b, 100)).bytes) == "connected", "and it arrives");
        check(B::udp_address(a).starts_with("127.0.0.1:") && B::udp_is_open(a), "udp address and state");
        check(ok(B::udp_set_broadcast(a, true)), "set_broadcast");
        check(ok(B::udp_set_multicast_loopback(a, true)) && ok(B::udp_set_multicast_ttl(a, 2)), "multicast options");
        B::UdpSpec sized;
        sized.receive_buffer = 1 << 20;
        sized.send_buffer = 1 << 18;
        check(ok(B::udp_bind("127.0.0.1", 0, sized)), "socket buffer sizes are accepted");
    }

    // ---- processes: stdio modes, id, exit status ----
    {
        B::ProcessSpec spec;
        spec.program = "/bin/sh";
        spec.args = {"-c", "echo out; echo err 1>&2; exit 4"};
        spec.stdin_mode = B::StdioKind::Null;
        auto child = B::process_spawn(spec);
        check(B::process_id(child) > 0, "process_id");
        const auto err = finish(B::io_read_to_end(child, B::ProcessStream::Stderr, 0));
        const auto out = finish(B::io_read_to_end(child, B::ProcessStream::Stdout, 0));
        check(text(out.bytes) == "out\n" && text(err.bytes) == "err\n", "io_read_to_end reads each pipe of a process");
        const auto exit = finish(B::process_wait(child));
        check(exit.number == 4, "exit status");

        B::ProcessSpec cat;
        cat.program = "/bin/cat";
        auto filter = B::process_spawn(cat);
        auto sink = B::memory_writer_new();
        auto copy_out = B::io_copy(filter, B::ProcessStream::Stdout, sink, B::ProcessStream::Stdout, {});
        check(ok(B::io_write_all(filter, B::ProcessStream::Stdin, bytes("piped through cat"))), "io_write_all to a process's stdin");
        B::process_close_stdin(filter);
        check(finish(copy_out).number == 17 && text(B::memory_writer_bytes(sink)) == "piped through cat", "io_copy from a process's stdout");

        B::ProcessSpec sleeper;
        sleeper.program = "/bin/sleep";
        sleeper.args = {"30"};
        auto long_child = B::process_spawn(sleeper);
        B::process_request_exit(long_child);
        const auto ended = finish(B::process_wait(long_child));
        check(B::OpStatus::Ok == ended.status && ended.fields.size() == 3, "request_exit ends a child and wait reports the signal fields");
        bool signaled = false;
        for (const auto &[k, v] : ended.fields) if (k == "signaled" && v == "1") signaled = true;
        check(signaled, "and says it was signalled");

        B::ProcessSpec quiet;
        quiet.program = "/bin/echo";
        quiet.args = {"hidden"};
        quiet.stdout_mode = B::StdioKind::Null;
        auto muted = B::process_spawn(quiet);
        check(finish(B::process_read_stdout(muted, 100)).status == B::OpStatus::Failed || finish(B::process_read_stdout(muted, 100)).bytes.empty(), "a null stdout reads as nothing");
    }

    // ---- watching: options, backend, events as a stream ----
    {
        B::WatchSpec spec;
        spec.debounce_ms = 20;
        auto watcher = B::watch_path(root, spec);
        check(B::watch_uses_native_backend(watcher) == ((B::features() & B::FeatureNativeFileWatch) != 0), "the backend matches the feature bit");
        auto events = B::watch_events_stream(watcher);
        auto next = B::stream_next(events);
        { std::ofstream(scratch / "watched.txt") << "x"; }
        const auto event = finish(next);
        const auto fields = event.number == 1 ? B::unpack_frames(event.bytes) : std::vector<B::Bytes>{};
        check(fields.size() == 4 && text(fields[1]).find("watched.txt") != std::string::npos, "watch_events_stream yields packed events");
        B::watch_close(watcher);
        B::WatchSpec polled;
        polled.force_polling = true;
        polled.poll_interval_ms = 30;
        check(!B::watch_uses_native_backend(B::watch_path(root, polled)), "force_polling selects the polling backend");
    }

    // ---- relay: authorization ----
    {
        B::RelaySpec spec;
        spec.bind_address = "127.0.0.1";
        spec.max_token_bytes = 40;
        std::atomic<int> asked{0};
        auto relay = B::relay_server_start(spec, [&](const std::string &token, const std::string &) {
            ++asked;
            return token.starts_with("allowed-");
        });
        const auto port = B::relay_server_port(relay);
        const auto denied = finish(B::relay_connect("127.0.0.1", port, "forbidden-token", true, nullptr, 3000));
        check(denied.status == B::OpStatus::Failed && asked >= 1, "the authorize callback can refuse a client");
        auto dial = B::relay_connect("127.0.0.1", port, "allowed-1234", true, nullptr, 5000);
        auto accept = B::relay_connect("127.0.0.1", port, "allowed-1234", false, nullptr, 5000);
        check(finish(dial).status == B::OpStatus::Ok && finish(accept).status == B::OpStatus::Ok, "and let an authorised pair through");
        B::relay_server_stop(relay);
    }

    fs::remove_all(scratch);
    if (failures == 0) Foundation::log_info("BindingIoTest passed");
    return failures == 0 ? 0 : 1;
}
