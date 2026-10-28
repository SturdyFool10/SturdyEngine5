#include <Async/BindingInternal.hpp>

#include <Async/File.hpp>
#include <Async/FileHandle.hpp>
#include <Async/IoHandler.hpp>
#include <Async/Io.hpp>
#include <Async/Process.hpp>
#include <Async/Stream.hpp>

#include <limits>
#include <mutex>

namespace SFT::Async {

    namespace Detail {
        /// A reader and/or writer of any supported kind behind one type, so the generic algorithms in <Async/Io.hpp> can run on it.
        /// It keeps the wrapped object alive; the functions refer to it by reference.
        struct BindingErasedIo {
            using Chunk = std::expected<std::vector<std::byte>, IoError>;
            using Done = std::expected<void, IoError>;
            Binding::ObjectPtr keep;
            std::function<TaskHandle<Chunk>(std::size_t, CancellationToken)> read;
            std::function<TaskHandle<Done>(std::vector<std::byte>, CancellationToken)> write;
            std::function<TaskHandle<Done>()> shutdown;
        };
    } // namespace Detail

    template <>
    struct IoTraits<Detail::BindingErasedIo> {
        static TaskHandle<Detail::BindingErasedIo::Chunk> read_some(Detail::BindingErasedIo &io, std::size_t n, CancellationToken token) {
            if (!io.read) return ready(Detail::BindingErasedIo::Chunk(std::unexpected(IoError{IoErrorCode::Unsupported, "this stream cannot be read"})));
            return io.read(n, std::move(token));
        }
        static TaskHandle<Detail::BindingErasedIo::Done> write_all(Detail::BindingErasedIo &io, std::vector<std::byte> data, CancellationToken token) {
            if (!io.write) return ready(Detail::BindingErasedIo::Done(std::unexpected(IoError{IoErrorCode::Unsupported, "this stream cannot be written"})));
            return io.write(std::move(data), std::move(token));
        }
        static TaskHandle<Detail::BindingErasedIo::Done> shutdown(Detail::BindingErasedIo &io) {
            if (!io.shutdown) return ready(Detail::BindingErasedIo::Done());
            return io.shutdown();
        }
    };

} // namespace SFT::Async

namespace SFT::Async::Binding {

    using namespace Detail;
    using Erased = SFT::Async::Detail::BindingErasedIo;

    namespace {
        constexpr const char *kTcp = "tcp";
        constexpr const char *kFile = "file";
        constexpr const char *kProcess = "process";
        constexpr const char *kPipeEnd = "pipe_end";
        constexpr const char *kMemoryReader = "memory_reader";
        constexpr const char *kMemoryWriter = "memory_writer";
        constexpr const char *kBufferedReader = "buffered_reader";
        constexpr const char *kBufferedWriter = "buffered_writer";
        constexpr const char *kFramed = "framed";
        constexpr const char *kDirIter = "dir_iter";
        constexpr const char *kStream = "stream";

        bool is_kind(const ObjectPtr &object, const char *kind) { return object && std::strcmp(object->kind(), kind) == 0; }

        auto bytes_out = [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); };

        /// Wraps `object` for reading and/or writing; kinds that do not support a direction leave that function empty.
        std::shared_ptr<Erased> erase(const ObjectPtr &object, ProcessStream stream) {
            auto io = std::make_shared<Erased>();
            io->keep = object;
            if (is_kind(object, kTcp)) {
                auto &c = get<TcpConnection>(object, kTcp);
                io->read = [&c](std::size_t n, CancellationToken t) { return IoTraits<TcpConnection>::read_some(c, n, std::move(t)); };
                io->write = [&c](std::vector<std::byte> d, CancellationToken t) { return IoTraits<TcpConnection>::write_all(c, std::move(d), std::move(t)); };
                io->shutdown = [&c] { return IoTraits<TcpConnection>::shutdown(c); };
            } else if (is_kind(object, kFile)) {
                auto &f = get<File>(object, kFile);
                io->read = [&f](std::size_t n, CancellationToken t) { return IoTraits<File>::read_some(f, n, std::move(t)); };
                io->write = [&f](std::vector<std::byte> d, CancellationToken t) { return IoTraits<File>::write_all(f, std::move(d), std::move(t)); };
                io->shutdown = [&f] { return IoTraits<File>::shutdown(f); };
            } else if (is_kind(object, kPipeEnd)) {
                auto &p = *get<std::shared_ptr<PipeEnd>>(object, kPipeEnd);
                io->read = [&p](std::size_t n, CancellationToken t) { return IoTraits<PipeEnd>::read_some(p, n, std::move(t)); };
                io->write = [&p](std::vector<std::byte> d, CancellationToken t) { return IoTraits<PipeEnd>::write_all(p, std::move(d), std::move(t)); };
                io->shutdown = [&p] { return IoTraits<PipeEnd>::shutdown(p); };
            } else if (is_kind(object, kMemoryReader)) {
                auto &r = *get<std::shared_ptr<MemoryReader>>(object, kMemoryReader);
                io->read = [&r](std::size_t n, CancellationToken t) { return IoTraits<MemoryReader>::read_some(r, n, std::move(t)); };
            } else if (is_kind(object, kMemoryWriter)) {
                auto &w = *get<std::shared_ptr<MemoryWriter>>(object, kMemoryWriter);
                io->write = [&w](std::vector<std::byte> d, CancellationToken t) { return IoTraits<MemoryWriter>::write_all(w, std::move(d), std::move(t)); };
                io->shutdown = [&w] { return IoTraits<MemoryWriter>::shutdown(w); };
            } else if (is_kind(object, kProcess)) {
                auto &p = get<Process>(object, kProcess);
                switch (stream) {
                    case ProcessStream::Stdout: io->read = [&p](std::size_t n, CancellationToken t) { return p.read_stdout(n, std::move(t)); }; break;
                    case ProcessStream::Stderr: io->read = [&p](std::size_t n, CancellationToken t) { return p.read_stderr(n, std::move(t)); }; break;
                    case ProcessStream::Stdin:
                        io->write = [&p](std::vector<std::byte> d, CancellationToken t) { return p.write_stdin(std::move(d), std::move(t)); };
                        io->shutdown = [&p] {
                            p.close_stdin();
                            return ready(Erased::Done());
                        };
                        break;
                }
            } else {
                throw BindingError(IoErrorCode::InvalidArgument, std::string("a '") + (object ? object->kind() : "null") + "' object is not a byte stream");
            }
            return io;
        }
        std::shared_ptr<Erased> erase_reader(const ObjectPtr &object, ProcessStream stream) {
            auto io = erase(object, stream);
            if (!io->read) throw BindingError(IoErrorCode::InvalidArgument, std::string("a '") + object->kind() + "' object cannot be read");
            return io;
        }
        std::shared_ptr<Erased> erase_writer(const ObjectPtr &object, ProcessStream stream) {
            auto io = erase(object, stream);
            if (!io->write) throw BindingError(IoErrorCode::InvalidArgument, std::string("a '") + object->kind() + "' object cannot be written");
            return io;
        }

        struct DirIterState {
            std::mutex mutex;
            DirectoryIterator iterator;
            DirIterState(std::string path, bool recursive) : iterator(std::move(path), recursive) {}
        };

        struct BufferedReaderState {
            std::shared_ptr<Erased> io;
            BufferedReader<Erased> reader;
            BufferedReaderState(std::shared_ptr<Erased> source, std::size_t chunk) : io(std::move(source)), reader(*io, chunk) {}
        };
        struct BufferedWriterState {
            std::shared_ptr<Erased> io;
            BufferedWriter<Erased> writer;
            BufferedWriterState(std::shared_ptr<Erased> sink, std::size_t capacity) : io(std::move(sink)), writer(*io, capacity) {}
        };

        /// Frames over a duplex stream, independent of the codec.
        struct FramedBase {
            virtual ~FramedBase() = default;
            virtual TaskHandle<std::expected<std::optional<Bytes>, IoError>> next(CancellationToken token) = 0;
            virtual TaskHandle<std::expected<void, IoError>> send(Bytes frame, CancellationToken token) = 0;
        };
        template <typename Codec>
        struct FramedImpl final : FramedBase {
            std::shared_ptr<Erased> io;
            Framed<Erased, Codec> framed;
            FramedImpl(std::shared_ptr<Erased> source, Codec codec) : io(std::move(source)), framed(*io, std::move(codec)) {}

            static Bytes bytes_of(const std::vector<std::byte> &v) { return from_native(v); }
            static Bytes bytes_of(const std::string &s) { return Bytes(s.begin(), s.end()); }

            TaskHandle<std::expected<std::optional<Bytes>, IoError>> next(CancellationToken token) override {
                using Out = std::expected<std::optional<Bytes>, IoError>;
                return SFT::Async::Detail::map_inline(framed.next(std::move(token)), [](std::expected<std::optional<typename Codec::frame_type>, IoError> r) -> Out {
                    if (!r) return std::unexpected(r.error());
                    if (!*r) return std::optional<Bytes>{};
                    return std::optional<Bytes>(bytes_of(**r));
                });
            }
            TaskHandle<std::expected<void, IoError>> send(Bytes frame, CancellationToken token) override {
                if constexpr (std::is_same_v<typename Codec::frame_type, std::string>) {
                    return framed.send(std::string(frame.begin(), frame.end()), std::move(token));
                } else {
                    return framed.send(to_native(frame), std::move(token));
                }
            }
        };
    } // namespace

    // =================================================================================================================
    // Whole-file helpers
    // =================================================================================================================
    OpPtr fs_read_file(const std::string &path, std::uint64_t max_bytes) {
        const std::size_t limit = max_bytes == 0 ? (std::numeric_limits<std::size_t>::max)() : static_cast<std::size_t>(max_bytes);
        return launch([&](CancellationToken token) { return read_file(path, std::move(token), limit); }, bytes_out);
    }
    OpPtr fs_read_range(const std::string &path, std::uint64_t offset, std::uint64_t byte_count) {
        return launch([&](CancellationToken token) { return read_file_range(path, offset, static_cast<std::size_t>(byte_count), std::move(token)); }, bytes_out);
    }
    OpPtr fs_write_file(const std::string &path, Bytes data, bool append) {
        return launch([&](CancellationToken token) { return write_file(path, to_native(data), append, std::move(token)); }, [](Value &) {});
    }
    OpPtr fs_size(const std::string &path) {
        return launch([&](CancellationToken token) { return file_size(path, std::move(token)); }, [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr fs_exists(const std::string &path) {
        return launch([&](CancellationToken token) { return file_exists(path, std::move(token)); }, [](bool exists, Value &out) { out.number = exists ? 1 : 0; });
    }
    OpPtr fs_create_directories(const std::string &path) {
        return launch([&](CancellationToken token) { return create_directories(path, std::move(token)); }, [](Value &) {});
    }
    OpPtr fs_remove_file(const std::string &path) {
        return launch([&](CancellationToken token) { return remove_file(path, std::move(token)); }, [](bool removed, Value &out) { out.number = removed ? 1 : 0; });
    }
    OpPtr fs_remove_directory(const std::string &path) {
        return launch([&](CancellationToken token) { return remove_directory(path, std::move(token)); }, [](bool removed, Value &out) { out.number = removed ? 1 : 0; });
    }
    OpPtr fs_copy(const std::string &source, const std::string &destination) {
        return launch([&](CancellationToken token) { return copy_file(source, destination, std::move(token)); }, [](Value &) {});
    }
    OpPtr fs_rename(const std::string &source, const std::string &destination) {
        return launch([&](CancellationToken token) { return rename_file(source, destination, std::move(token)); }, [](Value &) {});
    }
    OpPtr fs_list(const std::string &path, std::uint64_t max_entries, bool recursive) {
        const std::size_t limit = max_entries == 0 ? 4096 : static_cast<std::size_t>(max_entries);
        return launch([&](CancellationToken token) { return list_directory(path, limit, recursive, std::move(token)); }, [](std::vector<std::string> &&entries, Value &out) {
            std::vector<Bytes> frames;
            frames.reserve(entries.size());
            for (const auto &entry : entries) frames.emplace_back(entry.begin(), entry.end());
            out.number = static_cast<std::int64_t>(entries.size());
            out.bytes = pack_frames(frames);
        });
    }
    ObjectPtr dir_iter_new(const std::string &path, bool recursive) {
        return make_object<std::shared_ptr<DirIterState>>(kDirIter, std::make_shared<DirIterState>(path, recursive));
    }
    OpPtr dir_iter_next(const ObjectPtr &iterator) {
        auto state = get<std::shared_ptr<DirIterState>>(iterator, kDirIter);
        return launch(
            [state](CancellationToken) {
                return IoHandler::submit([state] {
                    std::lock_guard lock(state->mutex);
                    return state->iterator.next();
                });
            },
            [](std::optional<std::string> &&entry, Value &out) {
                out.number = entry ? 1 : 0;
                if (entry) out.text = std::move(*entry);
            });
    }

    // =================================================================================================================
    // Generic streams
    // =================================================================================================================
    OpPtr io_copy(const ObjectPtr &reader, ProcessStream reader_stream, const ObjectPtr &writer, ProcessStream writer_stream, const CopySpec &spec) {
        auto source = erase_reader(reader, reader_stream);
        auto sink = erase_writer(writer, writer_stream);
        CopyOptions options;
        options.chunk_bytes = spec.chunk_bytes == 0 ? 64 * 1024 : static_cast<std::size_t>(spec.chunk_bytes);
        options.limit = spec.limit == 0 ? (std::numeric_limits<std::uint64_t>::max)() : spec.limit;
        options.shutdown_writer = spec.shutdown_writer;
        return launch([&](CancellationToken token) { return copy(*source, *sink, options, std::move(token)); },
                      [source, sink](std::uint64_t total, Value &out) { out.number = static_cast<std::int64_t>(total); });
    }
    OpPtr io_read_to_end(const ObjectPtr &reader, ProcessStream stream, std::uint64_t max_bytes) {
        auto source = erase_reader(reader, stream);
        const std::size_t limit = max_bytes == 0 ? (std::numeric_limits<std::size_t>::max)() : static_cast<std::size_t>(max_bytes);
        return launch([&](CancellationToken token) { return read_to_end(*source, limit, std::move(token)); },
                      [source](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    OpPtr io_write_all(const ObjectPtr &writer, ProcessStream stream, Bytes data) {
        auto sink = erase_writer(writer, stream);
        return launch([&](CancellationToken token) { return write_all(*sink, to_native(data), std::move(token)); }, [sink](Value &) {});
    }

    ObjectPtr memory_reader_new(Bytes data) { return make_object<std::shared_ptr<MemoryReader>>(kMemoryReader, std::make_shared<MemoryReader>(to_native(data))); }
    ObjectPtr memory_writer_new() { return make_object<std::shared_ptr<MemoryWriter>>(kMemoryWriter, std::make_shared<MemoryWriter>()); }
    Bytes memory_writer_bytes(const ObjectPtr &writer) { return from_native(get<std::shared_ptr<MemoryWriter>>(writer, kMemoryWriter)->bytes()); }
    ChannelEnds memory_pipe_new(std::uint64_t chunks) {
        auto [a, b] = memory_pipe(chunks == 0 ? 8 : static_cast<std::size_t>(chunks));
        return {make_object<std::shared_ptr<PipeEnd>>(kPipeEnd, std::move(a)), make_object<std::shared_ptr<PipeEnd>>(kPipeEnd, std::move(b))};
    }

    ObjectPtr buffered_reader_new(const ObjectPtr &reader, ProcessStream stream, std::uint64_t chunk_bytes) {
        return make_object<std::shared_ptr<BufferedReaderState>>(
            kBufferedReader, std::make_shared<BufferedReaderState>(erase_reader(reader, stream), chunk_bytes == 0 ? 16 * 1024 : static_cast<std::size_t>(chunk_bytes)));
    }
    OpPtr buffered_read_some(const ObjectPtr &reader, std::uint64_t max_bytes) {
        auto state = get<std::shared_ptr<BufferedReaderState>>(reader, kBufferedReader);
        return launch([state, max_bytes](CancellationToken token) { return state->reader.read_some(static_cast<std::size_t>(max_bytes), std::move(token)); }, bytes_out);
    }
    OpPtr buffered_read_exact(const ObjectPtr &reader, std::uint64_t count) {
        auto state = get<std::shared_ptr<BufferedReaderState>>(reader, kBufferedReader);
        return launch([state, count](CancellationToken token) { return state->reader.read_exact(static_cast<std::size_t>(count), std::move(token)); }, bytes_out);
    }
    OpPtr buffered_read_until(const ObjectPtr &reader, const std::string &delimiter, std::uint64_t max_bytes) {
        auto state = get<std::shared_ptr<BufferedReaderState>>(reader, kBufferedReader);
        return launch([state, delimiter, max_bytes](CancellationToken token) { return state->reader.read_until(delimiter, static_cast<std::size_t>(max_bytes), std::move(token)); },
                      [](std::optional<std::vector<std::byte>> &&record, Value &out) {
                          out.number = record ? 1 : 0;
                          if (record) out.bytes = from_native(*record);
                      });
    }
    OpPtr buffered_read_line(const ObjectPtr &reader, std::uint64_t max_bytes) {
        auto state = get<std::shared_ptr<BufferedReaderState>>(reader, kBufferedReader);
        return launch([state, max_bytes](CancellationToken token) { return state->reader.read_line(max_bytes == 0 ? 64 * 1024 : static_cast<std::size_t>(max_bytes), std::move(token)); },
                      [](std::optional<std::string> &&line, Value &out) {
                          out.number = line ? 1 : 0;
                          if (line) out.text = std::move(*line);
                      });
    }
    std::uint64_t buffered_reader_buffered(const ObjectPtr &reader) { return get<std::shared_ptr<BufferedReaderState>>(reader, kBufferedReader)->reader.buffered(); }

    ObjectPtr buffered_writer_new(const ObjectPtr &writer, ProcessStream stream, std::uint64_t capacity) {
        return make_object<std::shared_ptr<BufferedWriterState>>(
            kBufferedWriter, std::make_shared<BufferedWriterState>(erase_writer(writer, stream), capacity == 0 ? 16 * 1024 : static_cast<std::size_t>(capacity)));
    }
    OpPtr buffered_write(const ObjectPtr &writer, Bytes data) {
        auto state = get<std::shared_ptr<BufferedWriterState>>(writer, kBufferedWriter);
        return launch([state, data = std::move(data)](CancellationToken token) mutable { return state->writer.write(to_native(data), std::move(token)); }, [](Value &) {});
    }
    OpPtr buffered_flush(const ObjectPtr &writer) {
        auto state = get<std::shared_ptr<BufferedWriterState>>(writer, kBufferedWriter);
        return launch([state](CancellationToken token) { return state->writer.flush(std::move(token)); }, [](Value &) {});
    }
    OpPtr buffered_shutdown(const ObjectPtr &writer) {
        auto state = get<std::shared_ptr<BufferedWriterState>>(writer, kBufferedWriter);
        return launch([state](CancellationToken token) { return state->writer.shutdown(std::move(token)); }, [](Value &) {});
    }

    ObjectPtr framed_new(const ObjectPtr &io, FrameCodec codec, std::uint64_t max_frame_bytes) {
        auto stream = erase(io, ProcessStream::Stdout);
        if (!stream->read || !stream->write) throw BindingError(IoErrorCode::InvalidArgument, "framing needs a stream that can be read and written");
        std::shared_ptr<FramedBase> framed;
        if (codec == FrameCodec::Lines) {
            LinesCodec c;
            if (max_frame_bytes != 0) c.max_line_bytes = static_cast<std::size_t>(max_frame_bytes);
            framed = std::make_shared<FramedImpl<LinesCodec>>(std::move(stream), c);
        } else {
            LengthDelimitedCodec c;
            if (max_frame_bytes != 0) c.max_frame_bytes = static_cast<std::size_t>(max_frame_bytes);
            framed = std::make_shared<FramedImpl<LengthDelimitedCodec>>(std::move(stream), c);
        }
        return make_object<std::shared_ptr<FramedBase>>(kFramed, std::move(framed));
    }
    OpPtr framed_next(const ObjectPtr &framed) {
        auto f = get<std::shared_ptr<FramedBase>>(framed, kFramed);
        return launch([f](CancellationToken token) { return f->next(std::move(token)); }, [](std::optional<Bytes> &&frame, Value &out) {
            out.number = frame ? 1 : 0;
            if (frame) out.bytes = std::move(*frame);
        });
    }
    OpPtr framed_send(const ObjectPtr &framed, Bytes frame) {
        auto f = get<std::shared_ptr<FramedBase>>(framed, kFramed);
        return launch([f, frame = std::move(frame)](CancellationToken token) mutable { return f->send(std::move(frame), std::move(token)); }, [](Value &) {});
    }
    ObjectPtr framed_stream(const ObjectPtr &framed) {
        auto f = get<std::shared_ptr<FramedBase>>(framed, kFramed);
        auto stream = stream_from([f]() -> TaskHandle<std::optional<Bytes>> {
            Promise<std::optional<Bytes>> promise;
            auto pending = f->next({});
            pending.on_done([pending, promise]() mutable {
                try {
                    auto result = pending.wait();
                    if (!result) (void)promise.set_exception(std::make_exception_ptr(std::runtime_error(result.error().message)));
                    else (void)promise.set_value(std::move(*result));
                } catch (...) {
                    (void)promise.set_exception(std::current_exception());
                }
            });
            return promise.handle();
        });
        return make_object<Stream<Bytes>>(kStream, std::move(stream));
    }

} // namespace SFT::Async::Binding
