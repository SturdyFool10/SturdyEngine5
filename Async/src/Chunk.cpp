#include <Async/src/Chunk.hpp>


namespace SFT::Async::Detail {

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

