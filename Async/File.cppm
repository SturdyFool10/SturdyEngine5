module;

#pragma region Imports
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Async:File;

import :Runtime;
import :IoError;

using std::expected;
using std::span;
using std::string;
using std::vector;

export namespace SFT::Async {

    namespace Detail {
        [[nodiscard]] expected<vector<std::byte>, IoError> read_file_blocking(const string &path);
        [[nodiscard]] expected<void, IoError> write_file_blocking(const string &path, span<const std::byte> data, bool append);
    } // namespace Detail

    // Reads the whole file at `path` without blocking the calling thread — on `Rt` (defaults to
    // `DefaultRuntime`: a `Scheduler` worker natively, inline on Web). "Asynchronous" here means
    // exactly that: the read itself is a plain blocking `std::ifstream` read, just run off the
    // caller via the runtime's task mechanism, not true OS-level async I/O (no io_uring/IOCP/kqueue
    // reactor). For most engine use (asset loading, config, shader source) that distinction doesn't
    // matter; it does if you need thousands of reads in flight at once with minimal thread use.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file(string path) {
        return Rt::spawn([path = std::move(path)]() mutable {
            return Detail::read_file_blocking(path);
        });
    }

    // Writes `data` to `path` (overwriting the file unless `append` is set), same non-blocking-call
    // shape as `read_file()`.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto write_file(string path, vector<std::byte> data, bool append = false) {
        return Rt::spawn([path = std::move(path), data = std::move(data), append]() mutable {
            return Detail::write_file_blocking(path, data, append);
        });
    }

} // namespace SFT::Async
