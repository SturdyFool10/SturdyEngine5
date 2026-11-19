#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace SFT::Core {

    /// Reads a whole file with parallel positional reads spread across Grand Central Dispatch's global queue: several reads in
    /// flight at once so an NVMe drive's queues stay full and the page-cache copy uses more than one core, without creating a
    /// thread per call. This is the macOS fast path (the platform has neither io_uring nor DirectStorage). Returns `nullopt` on
    /// any failure so the caller falls back to ordinary reads.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_file_dispatch(const std::filesystem::path &path);

} // namespace SFT::Core
