#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Runtime.hpp>

namespace SFT::Async {

    /// How `File::open` treats the path. Combine with `|`. At least one of Read/Write/Append is required.
    enum class OpenMode : unsigned {
        Read = 1u << 0,
        Write = 1u << 1,
        /// Create the file if it does not exist (needs Write or Append).
        Create = 1u << 2,
        /// Discard existing contents (needs Write).
        Truncate = 1u << 3,
        /// Every write goes to the end of the file regardless of the cursor.
        Append = 1u << 4,
        /// Create the file, failing with `AlreadyExists` if it is already there (atomic; implies Write).
        CreateNew = 1u << 5,
    };

    [[nodiscard]] constexpr OpenMode operator|(OpenMode a, OpenMode b) noexcept {
        return static_cast<OpenMode>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }
    [[nodiscard]] constexpr bool has_mode(OpenMode set, OpenMode flag) noexcept {
        return (static_cast<unsigned>(set) & static_cast<unsigned>(flag)) != 0;
    }

    enum class SeekOrigin { Begin, Current, End };

    class File;

    namespace Detail {
        struct FileState;

        /// True where no io_uring path can exist (non-Linux, Web), so the fast-path code is not even instantiated.
#if defined(__linux__) && !defined(STURDY_PLATFORM_WEB)
        inline constexpr bool kFileUringUnavailable = false;
#else
        inline constexpr bool kFileUringUnavailable = true;
#endif

        [[nodiscard]] std::expected<File, IoError> file_open_blocking(const std::string &path, OpenMode mode);
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> file_read_blocking(
            const std::shared_ptr<FileState> &state, std::size_t max_bytes, const std::uint64_t *offset, CancellationToken cancellation);
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> file_read_to_end_blocking(
            const std::shared_ptr<FileState> &state, std::size_t max_bytes, CancellationToken cancellation);
        [[nodiscard]] std::expected<std::size_t, IoError> file_write_blocking(
            const std::shared_ptr<FileState> &state, std::span<const std::byte> data, const std::uint64_t *offset,
            CancellationToken cancellation);
        [[nodiscard]] std::expected<std::uint64_t, IoError> file_seek_blocking(
            const std::shared_ptr<FileState> &state, std::int64_t offset, SeekOrigin origin);
        [[nodiscard]] std::expected<std::uint64_t, IoError> file_handle_size_blocking(const std::shared_ptr<FileState> &state);
        [[nodiscard]] std::expected<void, IoError> file_set_len_blocking(const std::shared_ptr<FileState> &state, std::uint64_t length);
        [[nodiscard]] std::expected<void, IoError> file_sync_blocking(const std::shared_ptr<FileState> &state, bool data_only);
        /// io_uring fast paths for positional operations (Linux, when the ring could be created). Each returns an invalid
        /// handle when it cannot take the request -- caller falls back to the IO lane. Completion callbacks run on the
        /// ring's reaper thread, like the network reactor: hop with `then<Rt>` before doing real work.
        [[nodiscard]] bool file_uring_available() noexcept;
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> file_read_at_uring(
            const std::shared_ptr<FileState> &state, std::uint64_t offset, std::size_t max_bytes, const CancellationToken &cancellation);
        [[nodiscard]] TaskHandle<std::expected<std::size_t, IoError>> file_write_at_uring(
            const std::shared_ptr<FileState> &state, std::uint64_t offset, std::vector<std::byte> &data, const CancellationToken &cancellation);
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> file_sync_uring(const std::shared_ptr<FileState> &state, bool data_only);
        [[nodiscard]] bool file_is_open(const std::shared_ptr<FileState> &state) noexcept;
        void file_close(const std::shared_ptr<FileState> &state) noexcept;
    } // namespace Detail

    /// An open file for streaming and random access -- the counterpart of `tokio::fs::File`.
    ///
    /// Every operation runs on the IO lane and returns a `TaskHandle`. A handle serializes its operations:
    /// issue them in the order you need (await one before starting the next if they depend on the cursor).
    /// Positional calls (`read_at`/`write_at`) do not move the cursor. `close()` waits for an in-flight
    /// operation on this handle to finish, then later operations fail with `InvalidArgument`.
    /// The destructor closes the file without flushing to disk; call `sync` first if durability matters.
    class File {
      public:
        File() noexcept = default;
        ~File() noexcept { close(); }

        File(const File &) = delete;
        File &operator=(const File &) = delete;
        File(File &&other) noexcept : state_(std::move(other.state_)) {}
        File &operator=(File &&other) noexcept {
            if (this != &other) {
                close();
                state_ = std::move(other.state_);
            }
            return *this;
        }

        [[nodiscard]] bool is_open() const noexcept { return Detail::file_is_open(state_); }
        void close() noexcept { Detail::file_close(state_); }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto open(std::string path, OpenMode mode) {
            return spawn_io<Rt>([path = std::move(path), mode]() { return Detail::file_open_blocking(path, mode); });
        }

        /// Reads up to `max_bytes` from the cursor and advances it. An empty result means end of file.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto read(std::size_t max_bytes, CancellationToken cancellation = {}) {
            return spawn_io<Rt>([state = state_, max_bytes, cancellation]() {
                return Detail::file_read_blocking(state, max_bytes, nullptr, cancellation);
            });
        }

        /// Reads up to `max_bytes` at `offset` without touching the cursor; short at EOF. On Linux with io_uring, reads of up
        /// to 4 MiB are submitted to the kernel directly (no thread is occupied; they cannot be cancelled once submitted).
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto read_at(std::uint64_t offset, std::size_t max_bytes, CancellationToken cancellation = {}) {
            if constexpr (std::same_as<Rt, DefaultRuntime> && !Detail::kFileUringUnavailable) {
                if (auto fast = Detail::file_read_at_uring(state_, offset, max_bytes, cancellation); fast.valid()) return fast;
            }
            return spawn_io<Rt>([state = state_, offset, max_bytes, cancellation]() {
                return Detail::file_read_blocking(state, max_bytes, &offset, cancellation);
            });
        }

        /// Reads from the cursor to EOF. Fails with `InvalidArgument` (before reading further) if more than
        /// `max_bytes` remain, so a runaway file cannot exhaust memory.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto read_to_end(std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)(),
                                       CancellationToken cancellation = {}) {
            return spawn_io<Rt>([state = state_, max_bytes, cancellation]() {
                return Detail::file_read_to_end_blocking(state, max_bytes, cancellation);
            });
        }

        /// Writes all of `data` at the cursor (or the end, in Append mode), resolving to the byte count.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto write(std::vector<std::byte> data, CancellationToken cancellation = {}) {
            return spawn_io<Rt>([state = state_, data = std::move(data), cancellation]() {
                return Detail::file_write_blocking(state, data, nullptr, cancellation);
            });
        }

        /// Writes all of `data` at `offset` without touching the cursor. Not valid on files opened with Append.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto write_at(std::uint64_t offset, std::vector<std::byte> data, CancellationToken cancellation = {}) {
            if constexpr (std::same_as<Rt, DefaultRuntime> && !Detail::kFileUringUnavailable) {
                if (auto fast = Detail::file_write_at_uring(state_, offset, data, cancellation); fast.valid()) return fast;
            }
            return spawn_io<Rt>([state = state_, offset, data = std::move(data), cancellation]() {
                return Detail::file_write_blocking(state, data, &offset, cancellation);
            });
        }

        /// Moves the cursor and resolves to its new absolute position.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto seek(std::int64_t offset, SeekOrigin origin = SeekOrigin::Begin) {
            return spawn_io<Rt>([state = state_, offset, origin]() { return Detail::file_seek_blocking(state, offset, origin); });
        }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto size() {
            return spawn_io<Rt>([state = state_]() { return Detail::file_handle_size_blocking(state); });
        }

        /// Truncates or zero-extends the file to `length` bytes.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto set_len(std::uint64_t length) {
            return spawn_io<Rt>([state = state_, length]() { return Detail::file_set_len_blocking(state, length); });
        }

        /// Flushes to stable storage (fsync). `data_only` skips metadata where the platform allows (fdatasync).
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto sync(bool data_only = false) {
            if constexpr (std::same_as<Rt, DefaultRuntime> && !Detail::kFileUringUnavailable) {
                if (auto fast = Detail::file_sync_uring(state_, data_only); fast.valid()) return fast;
            }
            return spawn_io<Rt>([state = state_, data_only]() { return Detail::file_sync_blocking(state, data_only); });
        }

      private:
        friend std::expected<File, IoError> Detail::file_open_blocking(const std::string &path, OpenMode mode);

        explicit File(std::shared_ptr<Detail::FileState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::FileState> state_;
    };

} // namespace SFT::Async
