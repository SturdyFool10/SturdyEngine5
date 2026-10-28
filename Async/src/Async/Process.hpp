#pragma once

// Child processes (tokio::process): spawn, talk to the standard streams, wait, kill -- all on the network reactor, so
// a hundred children cost no threads. Windows and POSIX share this API.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {
    using Literals::operator""_MiB;

    enum class StdioMode {
        Null,    ///< connected to the null device (reads EOF / discards)
        Inherit, ///< shares this process's own stream
        Pipe,    ///< connected to the parent through `Process::write_stdin` / `read_stdout` / `read_stderr`
    };

    struct ProcessOptions {
        /// Executable name (looked up on PATH when it has no directory part) or a path.
        std::string program;
        std::vector<std::string> args;
        std::string working_directory;                  ///< empty: inherit
        /// Added to (or, with `clear_environment`, replacing) the environment the child starts with.
        std::map<std::string, std::string> environment;
        bool clear_environment = false;
        StdioMode stdin_mode = StdioMode::Null;
        StdioMode stdout_mode = StdioMode::Pipe;
        StdioMode stderr_mode = StdioMode::Pipe;
        /// Terminate the child when the `Process` object is destroyed while it still runs. Off by default (as in tokio).
        bool kill_on_drop = false;
    };

    struct ExitStatus {
        int code = 0;            ///< exit code (0..255); for a signalled child, the signal number
        bool signaled = false;   ///< ended by a signal (POSIX) rather than by exiting
        int signal = 0;
        [[nodiscard]] bool success() const noexcept { return !signaled && code == 0; }
    };

    namespace Detail {
        struct ProcessCore;
    }

    /// A running (or finished) child process. Move-only.
    class Process {
      public:
        Process() noexcept = default;
        ~Process() noexcept;
        Process(const Process &) = delete;
        Process &operator=(const Process &) = delete;
        Process(Process &&) noexcept = default;
        Process &operator=(Process &&other) noexcept;

        /// Starts the program. Failures to find or execute it are reported here.
        [[nodiscard]] static std::expected<Process, IoError> spawn(const ProcessOptions &options);

        [[nodiscard]] std::int64_t id() const noexcept;

        /// Writes all of `data` to the child's stdin (needs `stdin_mode == Pipe`). Queued writes keep their order.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> write_stdin(std::vector<std::byte> data, CancellationToken cancellation = {});
        /// Closes the child's stdin so it sees end-of-file.
        void close_stdin() noexcept;

        /// 1..`max_bytes` bytes from the child's stdout/stderr as soon as any are available; empty at end of stream.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_stdout(std::size_t max_bytes, CancellationToken cancellation = {});
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> read_stderr(std::size_t max_bytes, CancellationToken cancellation = {});

        /// Completes when the child has exited. May be called more than once; every caller gets the status.
        [[nodiscard]] TaskHandle<std::expected<ExitStatus, IoError>> wait(CancellationToken cancellation = {});

        /// Asks the child to exit (SIGTERM; terminates on Windows).
        void request_exit() noexcept;
        /// Kills the child (SIGKILL / TerminateProcess).
        void kill() noexcept;

      private:
        std::shared_ptr<Detail::ProcessCore> core_;
    };

    struct ProcessOutput {
        ExitStatus status;
        std::vector<std::byte> stdout_data;
        std::vector<std::byte> stderr_data;
        [[nodiscard]] std::string stdout_text() const { return std::string(reinterpret_cast<const char *>(stdout_data.data()), stdout_data.size()); }
        [[nodiscard]] std::string stderr_text() const { return std::string(reinterpret_cast<const char *>(stderr_data.data()), stderr_data.size()); }
    };

    struct RunLimits {
        /// Output beyond this (stdout + stderr) kills the child and fails with `LimitExceeded`.
        std::size_t max_output_bytes = 16_MiB;
        /// Zero: wait forever. Otherwise the child is killed and the call fails with `TimedOut`.
        std::chrono::milliseconds timeout{0};
    };

    /// Runs a program to completion: feeds `stdin_data`, collects stdout and stderr (bounded), kills it on timeout or
    /// excess output, and resolves with everything. The convenience layer over `Process` for command-line tools.
    /// stdout/stderr are piped regardless of the modes in `options`; stdin is piped when `stdin_data` is given.
    [[nodiscard]] TaskHandle<std::expected<ProcessOutput, IoError>> run_process(ProcessOptions options, std::vector<std::byte> stdin_data = {},
                                                                                RunLimits limits = {}, CancellationToken cancellation = {});

} // namespace SFT::Async
