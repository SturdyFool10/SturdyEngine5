#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Runtime.hpp>

namespace SFT::Async {

    namespace Detail {
        /// Reads file blocking from the associated source.
        ///
        /// @param path Filesystem path identifying the target resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`.
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> read_file_blocking(const std::string &path, std::size_t max_bytes, CancellationToken cancellation);
        [[nodiscard]] std::expected<std::size_t, IoError> file_size_blocking(const std::string &path, CancellationToken cancellation);
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> read_file_range_blocking(
            const std::string &path, std::uint64_t offset, std::size_t byte_count, CancellationToken cancellation);
        [[nodiscard]] std::expected<bool, IoError> file_exists_blocking(const std::string &path, CancellationToken cancellation);
        [[nodiscard]] std::expected<void, IoError> create_directories_blocking(const std::string &path, CancellationToken cancellation);
        [[nodiscard]] std::expected<bool, IoError> remove_file_blocking(const std::string &path, CancellationToken cancellation);
        [[nodiscard]] std::expected<void, IoError> copy_file_blocking(
            const std::string &source, const std::string &destination, CancellationToken cancellation);
        [[nodiscard]] std::expected<std::vector<std::string>, IoError> list_directory_blocking(
            const std::string &path, std::size_t max_entries, bool recursive, CancellationToken cancellation);
        [[nodiscard]] std::expected<void, IoError> rename_file_blocking(
            const std::string &source, const std::string &destination, CancellationToken cancellation);
        [[nodiscard]] std::expected<bool, IoError> remove_directory_blocking(const std::string &path, CancellationToken cancellation);
        /// Writes file blocking to the associated destination.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param data Data consumed or referenced by the operation.
        /// @param append `append` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] std::expected<void, IoError> write_file_blocking(
                    const std::string &path, std::span<const std::byte> data, bool append, CancellationToken cancellation);
    } // namespace Detail

    /// Reads file from the associated source.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto append_file(std::string path, std::vector<std::byte> data, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), data = std::move(data), cancellation]() mutable {
            return Detail::write_file_blocking(path, data, true, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto list_directory(std::string path, std::size_t max_entries = 4096, bool recursive = false,
                                      CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), max_entries, recursive, cancellation]() {
            return Detail::list_directory_blocking(path, max_entries, recursive, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto rename_file(std::string source, std::string destination, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([source = std::move(source), destination = std::move(destination), cancellation]() {
            return Detail::rename_file_blocking(source, destination, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto remove_directory(std::string path, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), cancellation]() {
            return Detail::remove_directory_blocking(path, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto file_exists(std::string path, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), cancellation]() {
            return Detail::file_exists_blocking(path, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto create_directories(std::string path, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), cancellation]() {
            return Detail::create_directories_blocking(path, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto remove_file(std::string path, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), cancellation]() {
            return Detail::remove_file_blocking(path, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto copy_file(std::string source, std::string destination, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([source = std::move(source), destination = std::move(destination), cancellation]() {
            return Detail::copy_file_blocking(source, destination, cancellation);
        });
    }

    class DirectoryIterator {
      public:
        using value_type = std::string;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;

        explicit DirectoryIterator(std::string path, bool recursive = false);
        DirectoryIterator(const DirectoryIterator &) = default;
        DirectoryIterator &operator=(const DirectoryIterator &) = default;
        DirectoryIterator(DirectoryIterator &&) noexcept = default;
        DirectoryIterator &operator=(DirectoryIterator &&) noexcept = default;

        [[nodiscard]] std::expected<std::optional<std::string>, IoError> next();
        [[nodiscard]] bool has_next() const noexcept;
        [[nodiscard]] const std::string &value() const;
        [[nodiscard]] DirectoryIterator begin() const { return *this; }
        [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
        [[nodiscard]] bool operator==(std::default_sentinel_t) const noexcept { return !has_next(); }
        [[nodiscard]] bool operator!=(std::default_sentinel_t sentinel) const noexcept { return !(*this == sentinel); }
        [[nodiscard]] friend bool operator==(std::default_sentinel_t sentinel, const DirectoryIterator &iterator) noexcept {
            return iterator == sentinel;
        }
        [[nodiscard]] friend bool operator!=(std::default_sentinel_t sentinel, const DirectoryIterator &iterator) noexcept {
            return !(iterator == sentinel);
        }
        [[nodiscard]] std::string operator*() const { return value(); }
        DirectoryIterator &operator++();
        void operator++(int) { ++(*this); }

      private:
        void advance() noexcept;
        bool recursive_ = false;
        std::filesystem::directory_iterator iterator_{};
        std::filesystem::directory_iterator end_{};
        std::filesystem::recursive_directory_iterator recursive_iterator_{};
        std::filesystem::recursive_directory_iterator recursive_end_{};
        std::string current_;
        std::optional<IoError> error_;
    };

    /// Opens a lazy, single-pass directory iterator. For IO-executor-backed materialization, use `list_directory`.
    [[nodiscard]] inline DirectoryIterator list_dir_iter(std::string path, bool recursive = false) {
        return DirectoryIterator{std::move(path), recursive};
    }

    /// Reads up to `byte_count` bytes beginning at `offset`; a range extending past EOF returns a shorter vector.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file_range(std::string path, std::uint64_t offset, std::size_t byte_count,
                                       CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), offset, byte_count, cancellation]() {
            return Detail::read_file_range_blocking(path, offset, byte_count, cancellation);
        });
    }

    /// Retrieves a file's size without loading its contents.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto file_size(std::string path, CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), cancellation]() {
            return Detail::file_size_blocking(path, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file(std::string path, CancellationToken cancellation = {},
                                 std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)()) {
        return spawn_io<Rt>([path = std::move(path), max_bytes, cancellation]() mutable {
            return Detail::read_file_blocking(path, max_bytes, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file(std::string path, std::size_t max_bytes, CancellationToken cancellation = {}) {
        return read_file<Rt>(std::move(path), std::move(cancellation), max_bytes);
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto write_file(std::string path, std::vector<std::byte> data, bool append = false,
                                  CancellationToken cancellation = {}) {
        return spawn_io<Rt>([path = std::move(path), data = std::move(data), append, cancellation]() mutable {
            return Detail::write_file_blocking(path, data, append, cancellation);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto write_file(std::string path, std::vector<std::byte> data, CancellationToken cancellation) {
        return write_file<Rt>(std::move(path), std::move(data), false, std::move(cancellation));
    }

} // namespace SFT::Async
