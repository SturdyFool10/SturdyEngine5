#pragma once

#include <RHI/RHI.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace SFT::Core {

    // Windows fast-file-read backend for the texture-streaming pipeline, built on Microsoft's
    // DirectStorage API (see cmake/SturdyDependencies.cmake's sturdy_fetch_directstorage()). This is
    // the Windows sibling of the (not yet written) Linux io_uring backend referenced by
    // Decompression.hpp's own doc comment: both exist purely to get file bytes off disk faster/with
    // less CPU overhead than std::ifstream, handing the result to Core::decompress_gdeflate for the
    // actual (CPU-side) decompression.
    //
    // Deliberately scoped to memory-destination reads only -- every request built here uses
    // DSTORAGE_REQUEST_DESTINATION_MEMORY with DSTORAGE_QUEUE_DESC::Device left null. DirectStorage's
    // own header docs (dstorage.h's DSTORAGE_QUEUE_DESC::Device) confirm a null device is only valid
    // when every request on that queue targets memory, and is REQUIRED to be null in that case -- so
    // this file never creates a D3D12 device or any ID3D12Resource, and never touches DirectStorage's
    // other headline feature (GPU-side decompression into a D3D12 resource), which needs both. That
    // is a deliberate scoping decision, not a limitation of DirectStorage itself: the win captured
    // here is purely DirectStorage's fast asynchronous I/O path (bypassing the standard file cache,
    // batching reads via the OS's fastest available mechanism on supported NVMe setups), available
    // without any D3D12 involvement. dstorage.h transitively includes <d3d12.h> for the ID3D12Device*/
    // ID3D12Resource* pointer types its (unused, by this file) struct fields declare -- that is a
    // header-visibility detail, not a build dependency: this translation unit never links d3d12.lib
    // or calls a single D3D12 entry point.
    [[nodiscard]] bool direct_storage_available() noexcept;

    // Reads the whole contents of `path` via DirectStorage, blocking the calling thread until the
    // read completes. Synchronous by design, matching the synchronous std::ifstream-based read this
    // is meant to substitute for on Windows (see Engine::TextureStreamer's own class doc comment for
    // why file read stays on the calling thread in v1). Returns Unsupported if DirectStorage could
    // not be initialized on this system (missing runtime DLLs, no supported storage stack, ...) --
    // callers should fall back to a plain file read in that case, not treat it as fatal.
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> read_file_direct_storage(
        const std::filesystem::path &path);

} // namespace SFT::Core
