#include <Core/StreamingIo.hpp>

#include <Foundation/FileIo.hpp>

#include <utility>

#if defined(_WIN32)
#include <Core/IO/DirectStorage/DirectStorageBackend.hpp>
#elif defined(__linux__)
#include <Core/IO/IoUring/IoUringBackend.hpp>
#endif

namespace SFT::Core {

    std::optional<std::vector<std::byte>> read_file_accelerated(const std::filesystem::path &path) {
#if defined(_WIN32)
        if (auto bytes = read_file_direct_storage(path)) {
            return std::move(*bytes);
        }
#elif defined(__linux__)
        if (auto bytes = read_file_io_uring(path)) {
            return std::move(*bytes);
        }
#else
        (void)path;
#endif
        return std::nullopt;
    }

    void install_file_accelerator() {
        Foundation::Io::set_file_accelerator([](const std::filesystem::path &path) { return read_file_accelerated(path); });
    }

} // namespace SFT::Core
