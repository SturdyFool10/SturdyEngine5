#include <Foundation/FileIo.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SFT::Foundation::Io {

    namespace {

        constexpr intptr_t kInvalidHandle = -1;

        std::string os_error(std::string_view what, const std::filesystem::path &path) {
#if defined(_WIN32)
            const DWORD code = GetLastError();
            std::string message = std::system_category().message(static_cast<int>(code));
#else
            std::string message = std::strerror(errno);
#endif
            return std::string(what) + " '" + path.string() + "': " + message;
        }

        // ---- the platform layer: everything below the portable logic is these few functions ------------------------

#if defined(_WIN32)

        intptr_t native_open_read(const std::filesystem::path &path, AccessHint hint) noexcept {
            DWORD flags = FILE_ATTRIBUTE_NORMAL;
            if (hint == AccessHint::Sequential || hint == AccessHint::OneShot) {
                flags |= FILE_FLAG_SEQUENTIAL_SCAN;
            } else if (hint == AccessHint::Random) {
                flags |= FILE_FLAG_RANDOM_ACCESS;
            }
            const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, flags, nullptr);
            return h == INVALID_HANDLE_VALUE ? kInvalidHandle : reinterpret_cast<intptr_t>(h);
        }

        intptr_t native_open_write(const std::filesystem::path &path) noexcept {
            const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            return h == INVALID_HANDLE_VALUE ? kInvalidHandle : reinterpret_cast<intptr_t>(h);
        }

        void native_close(intptr_t handle) noexcept {
            CloseHandle(reinterpret_cast<HANDLE>(handle));
        }

        bool native_size(intptr_t handle, u64 &size) noexcept {
            LARGE_INTEGER value{};
            if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle), &value)) {
                return false;
            }
            size = static_cast<u64>(value.QuadPart);
            return true;
        }

        // One read at an offset; returns bytes read or -1.
        i64 native_pread(intptr_t handle, void *destination, usize count, u64 offset) noexcept {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD read = 0;
            const DWORD request = static_cast<DWORD>(std::min<usize>(count, 1u << 30));
            if (!ReadFile(reinterpret_cast<HANDLE>(handle), destination, request, &read, &overlapped)) {
                return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
            }
            return static_cast<i64>(read);
        }

        i64 native_write(intptr_t handle, const void *source, usize count) noexcept {
            DWORD written = 0;
            const DWORD request = static_cast<DWORD>(std::min<usize>(count, 1u << 30));
            if (!WriteFile(reinterpret_cast<HANDLE>(handle), source, request, &written, nullptr)) {
                return -1;
            }
            return static_cast<i64>(written);
        }

        bool native_seek(intptr_t handle, u64 offset) noexcept {
            LARGE_INTEGER value{};
            value.QuadPart = static_cast<LONGLONG>(offset);
            return SetFilePointerEx(reinterpret_cast<HANDLE>(handle), value, nullptr, FILE_BEGIN) != 0;
        }

        void native_advise(intptr_t, AccessHint) noexcept {} // fixed at open time on Windows
        void native_prefetch(intptr_t, u64, u64) noexcept {}
        void native_drop_cache(intptr_t) noexcept {}
        void native_preallocate(intptr_t, u64) noexcept {}

        class FileMapping final : public ByteBlob {
          public:
            ~FileMapping() override {
                if (view_ != nullptr) {
                    UnmapViewOfFile(view_);
                }
            }
            [[nodiscard]] static std::shared_ptr<FileMapping> create(const std::filesystem::path &path, AccessHint hint, std::string &error) {
                const intptr_t file = native_open_read(path, hint);
                if (file == kInvalidHandle) {
                    error = os_error("cannot open", path);
                    return nullptr;
                }
                u64 size = 0;
                if (!native_size(file, size)) {
                    error = os_error("cannot stat", path);
                    native_close(file);
                    return nullptr;
                }
                auto mapping = std::make_shared<FileMapping>();
                mapping->size_ = static_cast<usize>(size);
                if (size > 0) {
                    const HANDLE section = CreateFileMappingW(reinterpret_cast<HANDLE>(file), nullptr, PAGE_READONLY, 0, 0, nullptr);
                    if (section == nullptr) {
                        error = os_error("cannot map", path);
                        native_close(file);
                        return nullptr;
                    }
                    mapping->view_ = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
                    CloseHandle(section); // the view keeps the section alive
                    if (mapping->view_ == nullptr) {
                        error = os_error("cannot map", path);
                        native_close(file);
                        return nullptr;
                    }
                }
                native_close(file);
                return mapping;
            }
            [[nodiscard]] const std::byte *data() const noexcept override { return static_cast<const std::byte *>(view_); }
            [[nodiscard]] usize size() const noexcept override { return size_; }
            [[nodiscard]] bool mapped() const noexcept override { return true; }
            void prefetch(u64 offset, u64 length) const noexcept override {
                if (view_ == nullptr || offset >= size_) {
                    return;
                }
                WIN32_MEMORY_RANGE_ENTRY range{const_cast<std::byte *>(data()) + offset, static_cast<SIZE_T>(std::min<u64>(length, size_ - offset))};
                PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
            }
            void release(u64 offset, u64 length) const noexcept override {
                if (view_ == nullptr || offset >= size_) {
                    return;
                }
                // Dropping a view's pages needs the section to be a working-set candidate: OfferVirtualMemory is for
                // private memory only, so the portable answer is to let the memory manager trim it. Nothing to do.
                (void)length;
            }

          private:
            void *view_ = nullptr;
            usize size_ = 0;
        };

#else // POSIX

        intptr_t native_open_read(const std::filesystem::path &path, AccessHint hint) noexcept {
            int flags = O_RDONLY;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            const int fd = ::open(path.c_str(), flags);
            if (fd < 0) {
                return kInvalidHandle;
            }
            // Advice that applies for the lifetime of the descriptor.
#if defined(POSIX_FADV_SEQUENTIAL) && !defined(__APPLE__)
            switch (hint) {
            case AccessHint::Sequential:
            case AccessHint::OneShot: ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL); break;
            case AccessHint::Random: ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM); break;
            case AccessHint::Normal: break;
            }
#elif defined(__APPLE__) && defined(F_RDAHEAD)
            ::fcntl(fd, F_RDAHEAD, hint == AccessHint::Random ? 0 : 1);
#else
            (void)hint;
#endif
            return static_cast<intptr_t>(fd);
        }

        intptr_t native_open_write(const std::filesystem::path &path) noexcept {
            int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            const int fd = ::open(path.c_str(), flags, 0644);
            return fd < 0 ? kInvalidHandle : static_cast<intptr_t>(fd);
        }

        void native_close(intptr_t handle) noexcept { ::close(static_cast<int>(handle)); }

        bool native_size(intptr_t handle, u64 &size) noexcept {
            struct stat info{};
            if (::fstat(static_cast<int>(handle), &info) != 0) {
                return false;
            }
            size = static_cast<u64>(info.st_size);
            return true;
        }

        i64 native_pread(intptr_t handle, void *destination, usize count, u64 offset) noexcept {
            const usize request = std::min<usize>(count, 1u << 30);
            for (;;) {
                const ssize_t got = ::pread(static_cast<int>(handle), destination, request, static_cast<off_t>(offset));
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                return static_cast<i64>(got);
            }
        }

        i64 native_write(intptr_t handle, const void *source, usize count) noexcept {
            const usize request = std::min<usize>(count, 1u << 30);
            for (;;) {
                const ssize_t put = ::write(static_cast<int>(handle), source, request);
                if (put < 0 && errno == EINTR) {
                    continue;
                }
                return static_cast<i64>(put);
            }
        }

        bool native_seek(intptr_t handle, u64 offset) noexcept {
            return ::lseek(static_cast<int>(handle), static_cast<off_t>(offset), SEEK_SET) >= 0;
        }

        void native_advise(intptr_t handle, AccessHint hint) noexcept {
#if defined(POSIX_FADV_SEQUENTIAL) && !defined(__APPLE__)
            const int advice = hint == AccessHint::Random ? POSIX_FADV_RANDOM : hint == AccessHint::Normal ? POSIX_FADV_NORMAL : POSIX_FADV_SEQUENTIAL;
            ::posix_fadvise(static_cast<int>(handle), 0, 0, advice);
#elif defined(__APPLE__) && defined(F_RDAHEAD)
            ::fcntl(static_cast<int>(handle), F_RDAHEAD, hint == AccessHint::Random ? 0 : 1);
#else
            (void)handle;
            (void)hint;
#endif
        }

        void native_prefetch(intptr_t handle, u64 offset, u64 length) noexcept {
#if defined(POSIX_FADV_WILLNEED) && !defined(__APPLE__)
            ::posix_fadvise(static_cast<int>(handle), static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_WILLNEED);
#elif defined(__APPLE__) && defined(F_RDADVISE)
            radvisory range{static_cast<off_t>(offset), static_cast<int>(std::min<u64>(length, 1u << 30))};
            ::fcntl(static_cast<int>(handle), F_RDADVISE, &range);
#else
            (void)handle;
            (void)offset;
            (void)length;
#endif
        }

        void native_drop_cache(intptr_t handle) noexcept {
#if defined(POSIX_FADV_DONTNEED) && !defined(__APPLE__)
            ::posix_fadvise(static_cast<int>(handle), 0, 0, POSIX_FADV_DONTNEED);
#else
            (void)handle;
#endif
        }

        void native_preallocate(intptr_t handle, u64 bytes) noexcept {
#if defined(__linux__)
            if (bytes > 0) {
                ::posix_fallocate(static_cast<int>(handle), 0, static_cast<off_t>(bytes));
            }
#else
            (void)handle;
            (void)bytes;
#endif
        }

        class FileMapping final : public ByteBlob {
          public:
            ~FileMapping() override {
                if (view_ != nullptr) {
                    ::munmap(view_, size_);
                }
            }
            [[nodiscard]] static std::shared_ptr<FileMapping> create(const std::filesystem::path &path, AccessHint hint, std::string &error) {
                const intptr_t file = native_open_read(path, hint);
                if (file == kInvalidHandle) {
                    error = os_error("cannot open", path);
                    return nullptr;
                }
                u64 size = 0;
                if (!native_size(file, size)) {
                    error = os_error("cannot stat", path);
                    native_close(file);
                    return nullptr;
                }
                auto mapping = std::make_shared<FileMapping>();
                mapping->size_ = static_cast<usize>(size);
                if (size > 0) {
                    void *view = ::mmap(nullptr, mapping->size_, PROT_READ, MAP_PRIVATE, static_cast<int>(file), 0);
                    if (view == MAP_FAILED) {
                        error = os_error("cannot map", path);
                        native_close(file);
                        return nullptr;
                    }
                    mapping->view_ = view;
                    const int advice = hint == AccessHint::Random ? MADV_RANDOM : (hint == AccessHint::Normal ? MADV_NORMAL : MADV_SEQUENTIAL);
                    ::madvise(view, mapping->size_, advice);
                }
                native_close(file); // the mapping outlives the descriptor
                return mapping;
            }
            [[nodiscard]] const std::byte *data() const noexcept override { return static_cast<const std::byte *>(view_); }
            [[nodiscard]] usize size() const noexcept override { return size_; }
            [[nodiscard]] bool mapped() const noexcept override { return true; }
            void prefetch(u64 offset, u64 length) const noexcept override {
                const auto range = page_range(offset, length);
                if (range.second > 0) {
                    ::madvise(range.first, range.second, MADV_WILLNEED);
                }
            }
            void release(u64 offset, u64 length) const noexcept override {
                const auto range = page_range(offset, length);
                if (range.second > 0) {
                    // MADV_DONTNEED on a private read-only file mapping simply drops the pages; they fault back in from
                    // the file if touched again.
                    ::madvise(range.first, range.second, MADV_DONTNEED);
                }
            }

          private:
            /// The page-aligned span inside [offset, offset+length): partial pages at either end are left alone.
            [[nodiscard]] std::pair<void *, usize> page_range(u64 offset, u64 length) const noexcept {
                if (view_ == nullptr || offset >= size_) {
                    return {nullptr, 0};
                }
                static const u64 page = static_cast<u64>(::sysconf(_SC_PAGESIZE));
                const u64 end = std::min<u64>(offset + length, size_);
                const u64 begin = (offset + page - 1) / page * page;
                const u64 stop = end / page * page;
                if (stop <= begin) {
                    return {nullptr, 0};
                }
                return {static_cast<std::byte *>(view_) + begin, static_cast<usize>(stop - begin)};
            }

            void *view_ = nullptr;
            usize size_ = 0;
        };

#endif // platform layer

        class VectorBlob final : public ByteBlob {
          public:
            explicit VectorBlob(std::vector<std::byte> bytes) noexcept : bytes_(std::move(bytes)) {}
            [[nodiscard]] const std::byte *data() const noexcept override { return bytes_.data(); }
            [[nodiscard]] usize size() const noexcept override { return bytes_.size(); }

          private:
            std::vector<std::byte> bytes_;
        };

        // ---- accelerator hook -------------------------------------------------------------------------------------

        struct AcceleratorSlot {
            std::mutex mutex;
            FileAccelerator accelerator;
        };

        AcceleratorSlot &accelerator_slot() {
            static AcceleratorSlot slot;
            return slot;
        }

        std::optional<std::vector<std::byte>> try_accelerated(const std::filesystem::path &path) {
            FileAccelerator accelerator;
            {
                AcceleratorSlot &slot = accelerator_slot();
                std::scoped_lock lock(slot.mutex);
                accelerator = slot.accelerator;
            }
            return accelerator ? accelerator(path) : std::nullopt;
        }

        /// Fills `destination` from `handle`; false when the file ends early or a read fails.
        bool read_exact(intptr_t handle, std::byte *destination, u64 size) noexcept {
            u64 done = 0;
            while (done < size) {
                const i64 got = native_pread(handle, destination + done, static_cast<usize>(size - done), done);
                if (got <= 0) {
                    return false;
                }
                done += static_cast<u64>(got);
            }
            return true;
        }

    } // namespace

    // ---- ByteBlob / whole-file reads ---------------------------------------------------------------------------------

    std::shared_ptr<const ByteBlob> ByteBlob::from_vector(std::vector<std::byte> bytes) {
        return std::make_shared<VectorBlob>(std::move(bytes));
    }

    void set_file_accelerator(FileAccelerator accelerator) {
        AcceleratorSlot &slot = accelerator_slot();
        std::scoped_lock lock(slot.mutex);
        slot.accelerator = std::move(accelerator);
    }

    std::expected<std::vector<std::byte>, std::string> read_file(const std::filesystem::path &path, AccessHint hint) {
        const intptr_t handle = native_open_read(path, hint);
        if (handle == kInvalidHandle) {
            return std::unexpected(os_error("cannot open", path));
        }
        u64 size = 0;
        if (!native_size(handle, size)) {
            const std::string error = os_error("cannot stat", path);
            native_close(handle);
            return std::unexpected(error);
        }
        if (size >= kAcceleratorMinimumBytes) {
            native_close(handle);
            if (auto fast = try_accelerated(path)) {
                return std::move(*fast);
            }
            const intptr_t reopened = native_open_read(path, hint);
            if (reopened == kInvalidHandle) {
                return std::unexpected(os_error("cannot open", path));
            }
            return [&]() -> std::expected<std::vector<std::byte>, std::string> {
                std::vector<std::byte> bytes(static_cast<usize>(size));
                const bool ok = read_exact(reopened, bytes.data(), size);
                if (hint == AccessHint::OneShot) {
                    native_drop_cache(reopened);
                }
                native_close(reopened);
                if (!ok) {
                    return std::unexpected("failed to read '" + path.string() + "'");
                }
                return bytes;
            }();
        }
        std::vector<std::byte> bytes(static_cast<usize>(size));
        const bool ok = read_exact(handle, bytes.data(), size);
        native_close(handle);
        if (!ok) {
            return std::unexpected("failed to read '" + path.string() + "'");
        }
        return bytes;
    }

    std::expected<SharedBlob, std::string> open_blob(const std::filesystem::path &path, AccessHint hint, usize map_threshold) {
        std::error_code ec;
        const u64 size = std::filesystem::file_size(path, ec);
        if (!ec && size >= map_threshold) {
            std::string error;
            if (auto mapping = FileMapping::create(path, hint, error)) {
                return SharedBlob(std::move(mapping));
            }
            // Fall through: some file systems (network shares, FUSE) refuse to map; a plain read still works.
        }
        auto bytes = read_file(path, hint);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return ByteBlob::from_vector(std::move(*bytes));
    }

    std::expected<std::string, std::string> read_text_file(const std::filesystem::path &path) {
        auto bytes = read_file(path, AccessHint::OneShot);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
    }

    // ---- FileReader ----------------------------------------------------------------------------------------------------

    FileReader::FileReader() noexcept = default;

    FileReader::FileReader(FileReader &&other) noexcept : handle_(std::exchange(other.handle_, kInvalidHandle)), size_(other.size_) {}

    FileReader &FileReader::operator=(FileReader &&other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, kInvalidHandle);
            size_ = other.size_;
        }
        return *this;
    }

    FileReader::~FileReader() { close(); }

    void FileReader::close() noexcept {
        if (handle_ != kInvalidHandle) {
            native_close(handle_);
            handle_ = kInvalidHandle;
        }
    }

    bool FileReader::is_open() const noexcept { return handle_ != kInvalidHandle; }

    std::expected<FileReader, std::string> FileReader::open(const std::filesystem::path &path, AccessHint hint) {
        FileReader reader;
        reader.handle_ = native_open_read(path, hint);
        if (reader.handle_ == kInvalidHandle) {
            return std::unexpected(os_error("cannot open", path));
        }
        if (!native_size(reader.handle_, reader.size_)) {
            return std::unexpected(os_error("cannot stat", path));
        }
        return reader;
    }

    usize FileReader::read_at(u64 offset, void *destination, usize count) const noexcept {
        if (handle_ == kInvalidHandle || offset >= size_) {
            return 0;
        }
        count = static_cast<usize>(std::min<u64>(count, size_ - offset));
        usize done = 0;
        auto *out = static_cast<std::byte *>(destination);
        while (done < count) {
            const i64 got = native_pread(handle_, out + done, count - done, offset + done);
            if (got <= 0) {
                break;
            }
            done += static_cast<usize>(got);
        }
        return done;
    }

    void FileReader::advise(AccessHint hint) const noexcept {
        if (handle_ != kInvalidHandle) {
            native_advise(handle_, hint);
        }
    }

    void FileReader::prefetch(u64 offset, u64 length) const noexcept {
        if (handle_ != kInvalidHandle) {
            native_prefetch(handle_, offset, length);
        }
    }

    // ---- FileWriter ----------------------------------------------------------------------------------------------------

    FileWriter::FileWriter() noexcept = default;

    FileWriter::FileWriter(FileWriter &&other) noexcept
        : handle_(std::exchange(other.handle_, kInvalidHandle)),
          final_path_(std::move(other.final_path_)),
          temp_path_(std::move(other.temp_path_)),
          atomic_(other.atomic_),
          buffer_(std::move(other.buffer_)),
          buffered_(std::exchange(other.buffered_, 0)),
          position_(other.position_),
          high_water_(other.high_water_),
          error_(std::move(other.error_)) {}

    FileWriter &FileWriter::operator=(FileWriter &&other) noexcept {
        if (this != &other) {
            abandon();
            handle_ = std::exchange(other.handle_, kInvalidHandle);
            final_path_ = std::move(other.final_path_);
            temp_path_ = std::move(other.temp_path_);
            atomic_ = other.atomic_;
            buffer_ = std::move(other.buffer_);
            buffered_ = std::exchange(other.buffered_, 0);
            position_ = other.position_;
            high_water_ = other.high_water_;
            error_ = std::move(other.error_);
        }
        return *this;
    }

    FileWriter::~FileWriter() { abandon(); }

    void FileWriter::abandon() noexcept {
        if (handle_ == kInvalidHandle) {
            return;
        }
        native_close(handle_);
        handle_ = kInvalidHandle;
        if (atomic_) {
            std::error_code ec;
            std::filesystem::remove(temp_path_, ec);
        }
    }

    std::expected<FileWriter, std::string> FileWriter::create(const std::filesystem::path &path, const WriteOptions &options) {
        FileWriter writer;
        writer.final_path_ = path;
        writer.atomic_ = options.atomic;
        writer.temp_path_ = options.atomic ? std::filesystem::path(path.string() + ".part") : path;
        writer.handle_ = native_open_write(writer.temp_path_);
        if (writer.handle_ == kInvalidHandle) {
            return std::unexpected(os_error("cannot create", writer.temp_path_));
        }
        native_preallocate(writer.handle_, options.expected_size);
        writer.buffer_.resize(std::max<usize>(options.buffer_bytes, 4096));
        return writer;
    }

    bool FileWriter::write_raw(const std::byte *bytes, usize count) {
        while (count > 0) {
            const i64 put = native_write(handle_, bytes, count);
            if (put <= 0) {
                error_ = os_error("write failed for", temp_path_);
                return false;
            }
            bytes += put;
            count -= static_cast<usize>(put);
            position_ += static_cast<u64>(put);
        }
        return true;
    }

    bool FileWriter::flush() {
        if (!ok() || handle_ == kInvalidHandle) {
            return false;
        }
        if (buffered_ > 0) {
            const usize count = std::exchange(buffered_, 0);
            if (!write_raw(buffer_.data(), count)) {
                return false;
            }
            high_water_ = std::max(high_water_, position_);
        }
        return true;
    }

    bool FileWriter::write(std::span<const std::byte> bytes) {
        if (!ok() || handle_ == kInvalidHandle) {
            return false;
        }
        // Big writes bypass the buffer so they go to the kernel in one call.
        if (bytes.size() >= buffer_.size()) {
            return flush() && write_raw(bytes.data(), bytes.size()) && ((high_water_ = std::max(high_water_, position_)), true);
        }
        if (buffered_ + bytes.size() > buffer_.size() && !flush()) {
            return false;
        }
        std::memcpy(buffer_.data() + buffered_, bytes.data(), bytes.size());
        buffered_ += bytes.size();
        return true;
    }

    bool FileWriter::seek(u64 offset) {
        if (!flush()) {
            return false;
        }
        if (!native_seek(handle_, offset)) {
            error_ = os_error("seek failed for", temp_path_);
            return false;
        }
        position_ = offset;
        return true;
    }

    std::expected<void, std::string> FileWriter::commit() {
        if (handle_ == kInvalidHandle) {
            return std::unexpected(error_.empty() ? std::string("the file was already committed or never opened") : error_);
        }
        if (!flush()) {
            const std::string error = error_;
            abandon();
            return std::unexpected(error);
        }
        native_close(handle_);
        handle_ = kInvalidHandle;
        if (atomic_) {
            std::error_code ec;
            std::filesystem::rename(temp_path_, final_path_, ec);
            if (ec) {
                std::filesystem::remove(temp_path_, ec);
                return std::unexpected("cannot move the finished file into place at '" + final_path_.string() + "'");
            }
            atomic_ = false;
        }
        return {};
    }

} // namespace SFT::Foundation::Io
