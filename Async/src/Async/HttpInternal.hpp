#pragma once

// Private: pieces shared by the HTTP/1.1 and HTTP/2 server paths.

#include <Async/FileHandle.hpp>
#include <Async/Http.hpp>
#include <Async/NetInternal.hpp>
#include <Async/Sync.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace SFT::Async::Detail {

    inline bool iequals_header(const std::string &a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    /// Reads `length` bytes of `path` from `offset` in 64 KiB pieces on the IO lane, so a cold disk never stalls a reactor thread.
    inline HttpBodyStream make_file_producer(std::string path, std::uint64_t offset, std::uint64_t length) {
        struct Cursor {
            std::string path;
            std::uint64_t offset;
            std::uint64_t remaining;
            std::shared_ptr<File> file;
        };
        auto cursor = std::make_shared<Cursor>(Cursor{std::move(path), offset, length, nullptr});
        return [cursor](CancellationToken token) -> TaskHandle<std::optional<std::string>> {
            if (cursor->remaining == 0) return ready(std::optional<std::string>{});
            auto read_next = [cursor, token]() -> TaskHandle<std::optional<std::string>> {
                const std::size_t want = static_cast<std::size_t>((std::min<std::uint64_t>)(cursor->remaining, 64 * 1024));
                return map_inline(cursor->file->read_at(cursor->offset, want, token), [cursor](Result<std::vector<std::byte>> chunk) {
                    if (!chunk || chunk->empty()) throw std::runtime_error("file changed or became unreadable while streaming");
                    cursor->offset += chunk->size();
                    cursor->remaining -= (std::min<std::uint64_t>)(cursor->remaining, chunk->size());
                    return std::optional<std::string>(std::string(reinterpret_cast<const char *>(chunk->data()), chunk->size()));
                });
            };
            if (cursor->file) return read_next();
            Promise<std::optional<std::string>> promise;
            auto opening = File::open(cursor->path, OpenMode::Read);
            opening.on_done([opening, cursor, promise, read_next]() mutable {
                try {
                    auto opened = opening.wait();
                    if (!opened) throw std::runtime_error("cannot open file for streaming");
                    cursor->file = std::make_shared<File>(std::move(*opened));
                    auto first = read_next();
                    first.on_done([first, promise]() mutable {
                        try {
                            (void)promise.set_value(first.wait());
                        } catch (...) {
                            (void)promise.set_exception(std::current_exception());
                        }
                    });
                } catch (...) {
                    (void)promise.set_exception(std::current_exception());
                }
            });
            return promise.handle();
        };
    }

    /// Resolves a file response's size and range before it reaches the reactor: missing file -> 404, bad range -> 416.
    inline void normalize_file_response(HttpResponseMessage &response) {
        if (response.body_file.empty()) return;
        std::error_code error;
        const std::uint64_t size = std::filesystem::file_size(response.body_file, error);
        if (error) {
            response = HttpResponseMessage{404, {}, "not found", false, {}, 0, std::nullopt, {}, {}};
            return;
        }
        if (response.body_offset > size) {
            response = HttpResponseMessage{416, {{"Content-Range", "bytes */" + std::to_string(size)}}, "range not satisfiable", false, {}, 0, std::nullopt, {}, {}};
            return;
        }
        const std::uint64_t available = size - response.body_offset;
        response.body_length = (std::min)(response.body_length.value_or(available), available);
    }

} // namespace SFT::Async::Detail
