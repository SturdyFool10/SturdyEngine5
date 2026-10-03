#pragma once

#include <Foundation/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// The one place the engine reads and writes files. Every loader (audio, images, models, shaders) goes through these so
/// the platform's fast paths are used everywhere without each caller knowing about them: access-pattern hints
/// (read-ahead for streaming, none for random access, no cache pollution for one-shot loads), memory mapping for large or
/// repeatedly-seeked files (so a file bigger than RAM costs address space, not memory), positional reads that are safe from
/// many threads at once, and an accelerator hook where Core plugs in io_uring / DirectStorage for bulk loads.
namespace SFT::Foundation::Io {

    /// How a file is about to be used; picks the kernel's read-ahead and caching behaviour.
    enum class AccessHint : u8 {
        Normal,     ///< No advice.
        Sequential, ///< Read front to back (a streaming decode, a whole-file load): aggressive read-ahead.
        Random,     ///< Jumped around (seeking in audio, container indexes): no read-ahead, it would be wasted.
        OneShot,    ///< Read once and thrown away (asset import): sequential, and the pages are dropped from the cache after.
    };

    /// Immutable bytes with an owner: a heap buffer or a file mapping. Cheap to share, so decoders, caches and streaming
    /// sources can all hold the same blob.
    class ByteBlob {
      public:
        virtual ~ByteBlob() = default;
        [[nodiscard]] virtual const std::byte *data() const noexcept = 0;
        [[nodiscard]] virtual usize size() const noexcept = 0;
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data(), size()}; }

        /// Tells the OS the range is about to be read (a no-op for heap blobs). Safe to call from any thread.
        virtual void prefetch(u64 /*offset*/, u64 /*length*/) const noexcept {}
        /// Tells the OS the range will not be needed soon so it can reclaim the pages (a no-op for heap blobs). The data stays
        /// readable; touching it again just faults it back in. This is what keeps a long streamed file from filling RAM.
        virtual void release(u64 /*offset*/, u64 /*length*/) const noexcept {}
        /// True when the bytes are a file mapping rather than a heap copy.
        [[nodiscard]] virtual bool mapped() const noexcept { return false; }

        /// Wraps a vector (moved in) as a blob.
        [[nodiscard]] static std::shared_ptr<const ByteBlob> from_vector(std::vector<std::byte> bytes);
    };

    using SharedBlob = std::shared_ptr<const ByteBlob>;

    /// Reads a whole file with the fastest route available for its size: the registered accelerator for big files, otherwise
    /// one positional read straight into the destination with the right read-ahead advice.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(
        const std::filesystem::path &path, AccessHint hint = AccessHint::OneShot);

    /// Opens a file as a blob: small files are read into memory, files at or above `map_threshold` bytes are memory mapped
    /// (falling back to a read when the file system cannot map). Mapped files must not be truncated while in use.
    [[nodiscard]] std::expected<SharedBlob, std::string> open_blob(
        const std::filesystem::path &path, AccessHint hint = AccessHint::Normal, usize map_threshold = 1u << 20);

    /// Reads a whole file as text.
    [[nodiscard]] std::expected<std::string, std::string> read_text_file(const std::filesystem::path &path);

    /// Positional reads on an open file. `read_at` takes no shared cursor, so any number of threads can read one file at
    /// once (a streaming decoder and a seek from the game thread, say).
    class FileReader {
      public:
        FileReader() noexcept;
        FileReader(FileReader &&other) noexcept;
        FileReader &operator=(FileReader &&other) noexcept;
        FileReader(const FileReader &) = delete;
        FileReader &operator=(const FileReader &) = delete;
        ~FileReader();

        [[nodiscard]] static std::expected<FileReader, std::string> open(const std::filesystem::path &path, AccessHint hint = AccessHint::Normal);

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] u64 size() const noexcept { return size_; }
        /// Reads up to `count` bytes at `offset`; returns how many were read (fewer than asked only at the end of the file).
        /// Errors read as zero bytes.
        [[nodiscard]] usize read_at(u64 offset, void *destination, usize count) const noexcept;
        void advise(AccessHint hint) const noexcept;
        void prefetch(u64 offset, u64 length) const noexcept;

      private:
        void close() noexcept;

        intptr_t handle_ = -1;
        u64 size_ = 0;
    };

    struct WriteOptions {
        /// Write to a sibling temporary file and rename it over `path` in `commit()`. A crash or an abandoned writer then
        /// never leaves a truncated file behind. Strongly recommended for exports.
        bool atomic = true;
        /// Buffer size; writes smaller than this are coalesced into large system calls.
        usize buffer_bytes = 1u << 20;
        /// Hint for the final size so the file system can reserve contiguous space (Linux only, otherwise ignored).
        u64 expected_size = 0;
    };

    /// Buffered, seekable file writer for exports. Seeking back to patch a header (WAV sizes, FLAC streaminfo) is cheap and
    /// does not discard what is buffered beyond it.
    class FileWriter {
      public:
        FileWriter() noexcept;
        FileWriter(FileWriter &&other) noexcept;
        FileWriter &operator=(FileWriter &&other) noexcept;
        FileWriter(const FileWriter &) = delete;
        FileWriter &operator=(const FileWriter &) = delete;
        /// Abandons an uncommitted atomic write (the temporary file is removed).
        ~FileWriter();

        [[nodiscard]] static std::expected<FileWriter, std::string> create(const std::filesystem::path &path, const WriteOptions &options = {});

        /// Appends at the current position. After the first failure every further call fails and `error()` explains why.
        bool write(std::span<const std::byte> bytes);
        bool write(const void *bytes, usize count) { return write({static_cast<const std::byte *>(bytes), count}); }
        /// Moves the write position (absolute); flushes the buffer first.
        bool seek(u64 offset);
        [[nodiscard]] u64 tell() const noexcept { return position_ + buffered_; }
        /// Size of the file as written so far (the furthest position reached).
        [[nodiscard]] u64 size() const noexcept { return high_water_ > tell() ? high_water_ : tell(); }
        bool flush();
        /// Flushes, closes, and (for atomic writers) renames into place. The writer is unusable afterwards.
        [[nodiscard]] std::expected<void, std::string> commit();

        [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
        [[nodiscard]] const std::string &error() const noexcept { return error_; }

      private:
        void abandon() noexcept;
        bool write_raw(const std::byte *bytes, usize count);

        intptr_t handle_ = -1;
        std::filesystem::path final_path_;
        std::filesystem::path temp_path_;
        bool atomic_ = false;
        std::vector<std::byte> buffer_;
        usize buffered_ = 0;
        u64 position_ = 0;   // file offset of buffer_[0]
        u64 high_water_ = 0;
        std::string error_;
    };

    /// Hook for platform bulk-read paths. Core installs io_uring (Linux) / DirectStorage (Windows) here at startup; the
    /// callback returns nullopt when it cannot serve a request and the portable path runs instead.
    using FileAccelerator = std::function<std::optional<std::vector<std::byte>>(const std::filesystem::path &)>;
    void set_file_accelerator(FileAccelerator accelerator);
    /// Files below this size skip the accelerator (its setup cost beats the saving).
    inline constexpr usize kAcceleratorMinimumBytes = 256u * 1024u;

} // namespace SFT::Foundation::Io
