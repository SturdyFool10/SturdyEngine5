#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Async/src/IoError.hpp>
#include <Async/src/Runtime.hpp>

namespace SFT::Async {

    namespace Detail {
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> read_file_blocking(const std::string &path);
        [[nodiscard]] std::expected<void, IoError> write_file_blocking(const std::string &path, std::span<const std::byte> data, bool append);
    } // namespace Detail

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file(std::string path) {
        return Rt::spawn([path = std::move(path)]() mutable {
            return Detail::read_file_blocking(path);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto write_file(std::string path, std::vector<std::byte> data, bool append = false) {
        return Rt::spawn([path = std::move(path), data = std::move(data), append]() mutable {
            return Detail::write_file_blocking(path, data, append);
        });
    }

} // namespace SFT::Async
