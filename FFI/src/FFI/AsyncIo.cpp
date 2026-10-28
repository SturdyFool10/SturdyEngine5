/// C ABI for whole-file helpers, directories, packed frames and the generic byte-stream layer (copy, buffering, framing).
/// A thin translation of the matching part of `Async/Binding.hpp`.

#include <FFI/AsyncNetSupport.hpp>

using namespace SFT::Ffi::AsyncSupport;

namespace {
    B::ProcessStream stream_of(SturdyProcessStream stream) { return static_cast<B::ProcessStream>(stream); }
} // namespace

extern "C" {

// ----------------------------------------------------------------------------------------------------------------------
// Whole-file helpers
// ----------------------------------------------------------------------------------------------------------------------
#define STURDY_FS_OP(name, call)                                                                                                          \
    SturdyResult STURDY_ABI_CALL name {                                                                                                   \
        return run([&]() -> SturdyResult {                                                                                                \
            STURDY_OP_OUT(out_op);                                                                                                        \
            if (path == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be null");                                \
            return publish_op(call, out_op);                                                                                              \
        });                                                                                                                               \
    }

STURDY_FS_OP(sturdy_fs_read_file(const char *path, uint64_t max_bytes, SturdyAsyncOp *out_op), B::fs_read_file(path, max_bytes))
STURDY_FS_OP(sturdy_fs_read_range(const char *path, uint64_t offset, uint64_t byte_count, SturdyAsyncOp *out_op), B::fs_read_range(path, offset, byte_count))
STURDY_FS_OP(sturdy_fs_write_file(const char *path, const uint8_t *data, size_t size, SturdyBool append, SturdyAsyncOp *out_op),
             B::fs_write_file(path, bytes_of(data, size), append != STURDY_FALSE))
STURDY_FS_OP(sturdy_fs_size(const char *path, SturdyAsyncOp *out_op), B::fs_size(path))
STURDY_FS_OP(sturdy_fs_exists(const char *path, SturdyAsyncOp *out_op), B::fs_exists(path))
STURDY_FS_OP(sturdy_fs_create_directories(const char *path, SturdyAsyncOp *out_op), B::fs_create_directories(path))
STURDY_FS_OP(sturdy_fs_remove_file(const char *path, SturdyAsyncOp *out_op), B::fs_remove_file(path))
STURDY_FS_OP(sturdy_fs_remove_directory(const char *path, SturdyAsyncOp *out_op), B::fs_remove_directory(path))
STURDY_FS_OP(sturdy_fs_list(const char *path, uint64_t max_entries, SturdyBool recursive, SturdyAsyncOp *out_op), B::fs_list(path, max_entries, recursive != STURDY_FALSE))
#undef STURDY_FS_OP

SturdyResult STURDY_ABI_CALL sturdy_fs_copy(const char *source, const char *destination, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (source == nullptr || destination == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and destination must not be null");
        return publish_op(B::fs_copy(source, destination), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_fs_rename(const char *source, const char *destination, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (source == nullptr || destination == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and destination must not be null");
        return publish_op(B::fs_rename(source, destination), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dir_iter_create(const char *path, SturdyBool recursive, SturdyAsyncObject *out_iter) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_iter, "output pointer must not be null");
        if (path == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be null");
        *out_iter = publish_object(B::dir_iter_new(path, recursive != STURDY_FALSE));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dir_iter_next(SturdyAsyncObject iter, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(iter, object);
        return publish_op(B::dir_iter_next(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_is_open(SturdyAsyncObject file, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_open, "output pointer must not be null");
        STURDY_WITH_OBJECT(file, object);
        *out_open = B::file_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Packed frames
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_frames_pack(const SturdyBytesView *frames, uint32_t count, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (frames == nullptr && count != 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "frames must not be null");
        std::vector<B::Bytes> list;
        for (uint32_t i = 0; i < count; ++i) list.push_back(bytes_of(frames[i].data, frames[i].size));
        return copy_bytes_out(B::pack_frames(list), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frames_count(const uint8_t *packed, size_t size, uint32_t *out_count) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_count, "output pointer must not be null");
        *out_count = static_cast<uint32_t>(B::unpack_frames(bytes_of(packed, size)).size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frames_get(const uint8_t *packed, size_t size, uint32_t index, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        const auto frames = B::unpack_frames(bytes_of(packed, size));
        if (index >= frames.size()) return set_error(STURDY_ERROR_OUT_OF_RANGE, "frame index out of range");
        return copy_bytes_out(frames[index], buffer, capacity, out_length);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Generic byte streams
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_io_copy(SturdyAsyncObject reader, SturdyProcessStream reader_stream, SturdyAsyncObject writer, SturdyProcessStream writer_stream,
                                           const SturdyCopyOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(reader, source);
        STURDY_WITH_OBJECT(writer, sink);
        B::CopySpec spec;
        if (options != nullptr) {
            if (options->chunk_bytes != 0) spec.chunk_bytes = options->chunk_bytes;
            spec.limit = options->limit;
            spec.shutdown_writer = options->shutdown_writer != STURDY_FALSE;
        }
        return publish_op(B::io_copy(source, stream_of(reader_stream), sink, stream_of(writer_stream), spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_io_read_to_end(SturdyAsyncObject reader, SturdyProcessStream stream, uint64_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(reader, source);
        return publish_op(B::io_read_to_end(source, stream_of(stream), max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_io_write_all(SturdyAsyncObject writer, SturdyProcessStream stream, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(writer, sink);
        return publish_op(B::io_write_all(sink, stream_of(stream), bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_memory_reader_create(const uint8_t *data, size_t size, SturdyAsyncObject *out_reader) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_reader, "output pointer must not be null");
        *out_reader = publish_object(B::memory_reader_new(bytes_of(data, size)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_memory_writer_create(SturdyAsyncObject *out_writer) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_writer, "output pointer must not be null");
        *out_writer = publish_object(B::memory_writer_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_memory_writer_bytes(SturdyAsyncObject writer, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(writer, object);
        return copy_bytes_out(B::memory_writer_bytes(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_memory_pipe_create(uint64_t chunks, SturdyAsyncObject *out_end_a, SturdyAsyncObject *out_end_b) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_end_a, "output pointers must not be null");
        STURDY_REQUIRE(out_end_b, "output pointers must not be null");
        auto ends = B::memory_pipe_new(chunks);
        *out_end_a = publish_object(std::move(ends.sender));
        *out_end_b = publish_object(std::move(ends.receiver));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_reader_create(SturdyAsyncObject reader, SturdyProcessStream stream, uint64_t chunk_bytes, SturdyAsyncObject *out_reader) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_reader, "output pointer must not be null");
        STURDY_WITH_OBJECT(reader, source);
        *out_reader = publish_object(B::buffered_reader_new(source, stream_of(stream), chunk_bytes));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_read_some(SturdyAsyncObject reader, uint64_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(reader, object);
        return publish_op(B::buffered_read_some(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_read_exact(SturdyAsyncObject reader, uint64_t count, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(reader, object);
        return publish_op(B::buffered_read_exact(object, count), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_read_until(SturdyAsyncObject reader, const char *delimiter, uint64_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (delimiter == nullptr || *delimiter == '\0') return set_error(STURDY_ERROR_INVALID_ARGUMENT, "delimiter must not be empty");
        STURDY_WITH_OBJECT(reader, object);
        return publish_op(B::buffered_read_until(object, delimiter, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_read_line(SturdyAsyncObject reader, uint64_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(reader, object);
        return publish_op(B::buffered_read_line(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_reader_buffered(SturdyAsyncObject reader, uint64_t *out_bytes) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_bytes, "output pointer must not be null");
        STURDY_WITH_OBJECT(reader, object);
        *out_bytes = B::buffered_reader_buffered(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_writer_create(SturdyAsyncObject writer, SturdyProcessStream stream, uint64_t capacity, SturdyAsyncObject *out_writer) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_writer, "output pointer must not be null");
        STURDY_WITH_OBJECT(writer, sink);
        *out_writer = publish_object(B::buffered_writer_new(sink, stream_of(stream), capacity));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_write(SturdyAsyncObject writer, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(writer, object);
        return publish_op(B::buffered_write(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_flush(SturdyAsyncObject writer, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(writer, object);
        return publish_op(B::buffered_flush(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_buffered_shutdown(SturdyAsyncObject writer, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(writer, object);
        return publish_op(B::buffered_shutdown(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_framed_create(SturdyAsyncObject io, SturdyFrameCodec codec, uint64_t max_frame_bytes, SturdyAsyncObject *out_framed) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_framed, "output pointer must not be null");
        if (codec != STURDY_FRAME_LENGTH_DELIMITED && codec != STURDY_FRAME_LINES) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown codec");
        STURDY_WITH_OBJECT(io, object);
        *out_framed = publish_object(B::framed_new(object, static_cast<B::FrameCodec>(codec), max_frame_bytes));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_framed_next(SturdyAsyncObject framed, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(framed, object);
        return publish_op(B::framed_next(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_framed_send(SturdyAsyncObject framed, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(framed, object);
        return publish_op(B::framed_send(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_framed_stream(SturdyAsyncObject framed, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_stream, "output pointer must not be null");
        STURDY_WITH_OBJECT(framed, object);
        *out_stream = publish_object(B::framed_stream(object));
        return STURDY_OK;
    });
}

} // extern "C"
