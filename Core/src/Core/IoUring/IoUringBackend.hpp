#pragma once

#include <RHI/RHI.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace SFT::Core {

    // Linux fast-file-read backend for the texture-streaming pipeline, built directly on the
    // io_uring kernel UAPI (raw io_uring_setup/io_uring_enter syscalls + the shared submission/
    // completion ring memory mmap()ed from the kernel -- see IoUringBackend.cpp's own doc comment
    // for why this goes straight to the syscalls instead of a helper library) rather than
    // std::ifstream. This is the Linux sibling of DirectStorageBackend.hpp's Windows backend: same
    // role (get file bytes off disk faster/with less per-byte CPU overhead than a blocking read()),
    // same "hand the bytes to Core::decompress_gdeflate for actual decompression" contract, same
    // "Unsupported means fall back to plain file I/O, never treat it as fatal" error contract.
    [[nodiscard]] bool io_uring_available() noexcept;

    // Reads the whole contents of `path` via io_uring, blocking the calling thread until the read
    // completes (this backend keeps exactly one request in flight at a time -- see the .cpp's own
    // doc comment for why a deep ring buys nothing for this synchronous, one-file-at-a-time use
    // case). Returns Unsupported if io_uring could not be initialized on this system (syscalls
    // unavailable/blocked, e.g. an old kernel or a seccomp filter denying them) -- callers should
    // fall back to a plain file read in that case, not treat it as fatal.
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> read_file_io_uring(const std::filesystem::path &path);

} // namespace SFT::Core
