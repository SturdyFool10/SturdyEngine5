#include <Async/Chunk.hpp>


namespace SFT::Async::Detail {

    /// Performs the chunk bounds operation for `Detail` using the supplied arguments.
    ///
    /// @param size Requested or available size for the operation.
    /// @param chunk_count Number of elements or operations to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::vector<ChunkBounds> chunk_bounds(usize size, usize chunk_count) {
        std::vector<ChunkBounds> chunks;
        if (size == 0) {
            return chunks;
        }

        chunk_count = std::max<usize>(1, std::min(chunk_count, size));
        const usize base = size / chunk_count;
        const usize remainder = size % chunk_count;

        chunks.reserve(chunk_count);
        usize offset = 0;
        for (usize i = 0; i < chunk_count; ++i) {
            const usize count = base + (i < remainder ? 1 : 0);
            chunks.push_back(ChunkBounds{offset, offset + count});
            offset += count;
        }
        return chunks;
    }

} // namespace SFT::Async::Detail

