#include <Core/IO/Win32/OverlappedRead.hpp>

#if !defined(NOMINMAX)
    #define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>

#include <tracy/Tracy.hpp>

namespace SFT::Core {

    namespace {

        /// Requests per chunk, and chunks in flight. One megabyte at depth sixteen keeps an NVMe drive busy and still fits the
        /// DWORD length of `ReadFile`.
        constexpr DWORD kChunkBytes = 1u << 20;
        constexpr unsigned kQueueDepth = 16;

        /// An OVERLAPPED that remembers which part of the file it is reading, so a completion packet identifies its chunk.
        struct Chunk {
            OVERLAPPED overlapped{};
            std::uint64_t offset = 0;
            DWORD length = 0;
        };

        [[nodiscard]] bool issue(HANDLE file, std::byte *base, Chunk &chunk) noexcept {
            chunk.overlapped = OVERLAPPED{};
            chunk.overlapped.Offset = static_cast<DWORD>(chunk.offset & 0xffffffffu);
            chunk.overlapped.OffsetHigh = static_cast<DWORD>(chunk.offset >> 32);
            if (ReadFile(file, base + chunk.offset, chunk.length, nullptr, &chunk.overlapped)) {
                return true; // finished at once; its completion packet is still queued to the port
            }
            return GetLastError() == ERROR_IO_PENDING;
        }

    } // namespace

    std::optional<std::vector<std::byte>> read_file_overlapped(const std::filesystem::path &path) {
        ZoneScopedN("Core::read_file_overlapped");

        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
            CloseHandle(file);
            return std::nullopt;
        }
        const std::uint64_t file_size = static_cast<std::uint64_t>(size.QuadPart);

        const HANDLE port = CreateIoCompletionPort(file, nullptr, 0, 1);
        if (port == nullptr) {
            CloseHandle(file);
            return std::nullopt;
        }

        std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
        std::byte *const base = bytes.data();

        Chunk chunks[kQueueDepth];
        Chunk *free_chunks[kQueueDepth];
        unsigned free_count = kQueueDepth;
        for (unsigned i = 0; i < kQueueDepth; ++i) {
            free_chunks[i] = &chunks[i];
        }
        std::uint64_t next_offset = 0;
        unsigned inflight = 0;
        bool failed = false;

        while (inflight > 0 || (!failed && next_offset < file_size)) {
            while (!failed && free_count > 0 && next_offset < file_size) {
                Chunk &chunk = *free_chunks[--free_count];
                chunk.offset = next_offset;
                chunk.length = static_cast<DWORD>(std::min<std::uint64_t>(kChunkBytes, file_size - next_offset));
                if (!issue(file, base, chunk)) {
                    failed = true;
                    free_chunks[free_count++] = &chunk;
                    break;
                }
                next_offset += chunk.length;
                ++inflight;
            }
            if (inflight == 0) {
                break;
            }

            DWORD transferred = 0;
            ULONG_PTR key = 0;
            OVERLAPPED *completed = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(port, &transferred, &key, &completed, INFINITE);
            if (completed == nullptr) {
                // The port itself failed: requests may still be writing into `bytes`, so closing the handles (which cancels
                // them) and waiting is not possible here. Leak the buffer rather than free memory the kernel can still write.
                new std::vector<std::byte>(std::move(bytes));
                CloseHandle(port);
                CloseHandle(file);
                return std::nullopt;
            }
            --inflight;
            Chunk &chunk = *reinterpret_cast<Chunk *>(completed);
            if (!ok || transferred == 0) {
                failed = true;
            } else if (transferred < chunk.length) {
                chunk.offset += transferred;
                chunk.length -= transferred;
                if (!issue(file, base, chunk)) {
                    failed = true;
                } else {
                    ++inflight;
                    continue;
                }
            }
            free_chunks[free_count++] = &chunk;
        }

        CloseHandle(port);
        CloseHandle(file);
        if (failed) {
            return std::nullopt;
        }
        return bytes;
    }

} // namespace SFT::Core
