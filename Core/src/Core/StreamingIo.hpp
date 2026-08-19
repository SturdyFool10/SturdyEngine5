#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace SFT::Core {

    /// Attempts to read a file using the fastest platform-native streaming path compiled into Core.
    ///
    /// Returns `std::nullopt` when no accelerated path is available or when that path cannot service
    /// the request; callers may then fall back to ordinary filesystem I/O.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_file_accelerated(
        const std::filesystem::path &path);

} // namespace SFT::Core
