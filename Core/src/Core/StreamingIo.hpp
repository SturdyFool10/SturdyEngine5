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

    /// Registers `read_file_accelerated` with `Foundation::Io` so every whole-file load in the engine (audio, images, models)
    /// uses io_uring / DirectStorage for big files. Idempotent; called once at engine start.
    void install_file_accelerator();

} // namespace SFT::Core
