#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace SFT::Core {

    /// Reads a whole file with overlapped (asynchronous) Win32 reads completed through an I/O completion port: many reads in
    /// flight at once, so the drive's queue stays full, with no worker thread per request. This is the fast path on Windows
    /// machines without DirectStorage (no `dstorage.dll`, or a volume it cannot serve). Returns `nullopt` on any failure so the
    /// caller falls back to ordinary reads.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_file_overlapped(const std::filesystem::path &path);

} // namespace SFT::Core
