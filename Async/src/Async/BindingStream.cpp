#include <Async/BindingInternal.hpp>

#include <Async/Channel.hpp>
#include <Async/IoHandler.hpp>
#include <Async/ParIter.hpp>
#include <Async/Parallel.hpp>
#include <Async/Stream.hpp>
#include <Async/Tasks.hpp>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <span>

namespace SFT::Async::Binding {

    using namespace Detail;

    namespace {
        constexpr const char *kStream = "stream";
        constexpr const char *kChanReceiver = "channel_receiver";
        constexpr const char *kChanSender = "channel_sender";
        constexpr const char *kParScope = "par_scope";

        using BytesStream = Stream<Bytes>;

        void put_u32(Bytes &out, std::uint32_t value) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
        void put_u64(Bytes &out, std::uint64_t value) {
            for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }

        /// Takes the stream out of its object; the object is empty afterwards.
        BytesStream take(const ObjectPtr &object) {
            auto &stream = get<BytesStream>(object, kStream);
            if (!stream) throw BindingError(IoErrorCode::InvalidArgument, "the stream was already consumed");
            return std::move(stream);
        }
        ObjectPtr wrap(BytesStream stream) { return make_object<BytesStream>(kStream, std::move(stream)); }

        /// A sink that also stops when the op is cancelled (checked as items arrive).
        BytesStream until_cancelled(BytesStream stream, CancellationToken token) {
            if (!token.can_be_cancelled()) return stream;
            return std::move(stream).take_while([token](const Bytes &) { return !token.is_cancelled(); });
        }

        Bytes little_endian_nanos(Clock::time_point when) {
            Bytes out;
            put_u64(out, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count()));
            return out;
        }
    } // namespace

    Bytes pack_frames(const std::vector<Bytes> &frames) {
        Bytes out;
        put_u32(out, static_cast<std::uint32_t>(frames.size()));
        for (const auto &frame : frames) {
            put_u32(out, static_cast<std::uint32_t>(frame.size()));
            out.insert(out.end(), frame.begin(), frame.end());
        }
        return out;
    }
    std::vector<Bytes> unpack_frames(const Bytes &packed) {
        auto bad = [] { return BindingError(IoErrorCode::InvalidArgument, "malformed packed frames"); };
        auto read_u32 = [&](std::size_t at) -> std::uint32_t {
            if (at + 4 > packed.size()) throw bad();
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(packed[at + i]) << (8 * i);
            return value;
        };
        const std::uint32_t count = read_u32(0);
        std::vector<Bytes> frames;
        std::size_t at = 4;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t size = read_u32(at);
            at += 4;
            if (at + size > packed.size()) throw bad();
            frames.emplace_back(packed.begin() + static_cast<std::ptrdiff_t>(at), packed.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
        }
        if (at != packed.size()) throw bad();
        return frames;
    }

    // =================================================================================================================
    // Sources
    // =================================================================================================================
    ObjectPtr stream_from_channel(const ObjectPtr &receiver) { return wrap(stream_of_channel(get<Receiver<Bytes>>(receiver, kChanReceiver))); }
    ObjectPtr stream_of_items(const std::vector<Bytes> &items) { return wrap(stream_of(items)); }
    ObjectPtr stream_empty() { return wrap(SFT::Async::stream_empty<Bytes>()); }
    ObjectPtr stream_interval(std::int64_t period_ms) {
        if (period_ms <= 0) throw BindingError(IoErrorCode::InvalidArgument, "an interval needs a positive period");
        return wrap(SFT::Async::stream_interval(ms(period_ms)).map([](Clock::time_point when) { return little_endian_nanos(when); }));
    }
    ObjectPtr stream_generate(StreamSourceFn source) {
        if (!source) throw BindingError(IoErrorCode::InvalidArgument, "a source function is required");
        auto shared = std::make_shared<StreamSourceFn>(std::move(source));
        return wrap(stream_from([shared]() {
            return IoHandler::submit([shared]() -> std::optional<Bytes> {
                StreamMaybe item = (*shared)();
                if (!item.keep) return std::nullopt;
                return std::move(item.value);
            });
        }));
    }

    // =================================================================================================================
    // Adapters
    // =================================================================================================================
    ObjectPtr stream_map(const ObjectPtr &stream, StreamMapFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        return wrap(take(stream).map([fn = std::move(fn)](Bytes item) { return fn(std::move(item)); }));
    }
    ObjectPtr stream_filter(const ObjectPtr &stream, StreamPredicate pred) {
        if (!pred) throw BindingError(IoErrorCode::InvalidArgument, "a predicate is required");
        return wrap(take(stream).filter([pred = std::move(pred)](const Bytes &item) { return pred(item); }));
    }
    ObjectPtr stream_filter_map(const ObjectPtr &stream, StreamFilterMapFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        return wrap(take(stream).filter_map([fn = std::move(fn)](Bytes item) -> std::optional<Bytes> {
            StreamMaybe out = fn(std::move(item));
            if (!out.keep) return std::nullopt;
            return std::move(out.value);
        }));
    }
    ObjectPtr stream_inspect(const ObjectPtr &stream, StreamEachFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        return wrap(take(stream).inspect([fn = std::move(fn)](const Bytes &item) { fn(item); }));
    }
    ObjectPtr stream_take(const ObjectPtr &stream, std::uint64_t count) { return wrap(take(stream).take(static_cast<std::size_t>(count))); }
    ObjectPtr stream_skip(const ObjectPtr &stream, std::uint64_t count) { return wrap(take(stream).skip(static_cast<std::size_t>(count))); }
    ObjectPtr stream_take_while(const ObjectPtr &stream, StreamPredicate pred) {
        if (!pred) throw BindingError(IoErrorCode::InvalidArgument, "a predicate is required");
        return wrap(take(stream).take_while([pred = std::move(pred)](const Bytes &item) { return pred(item); }));
    }
    ObjectPtr stream_enumerate(const ObjectPtr &stream) {
        return wrap(take(stream).enumerate().map([](std::pair<std::size_t, Bytes> entry) {
            Bytes index;
            put_u64(index, entry.first);
            return pack_frames({std::move(index), std::move(entry.second)});
        }));
    }
    ObjectPtr stream_chunks(const ObjectPtr &stream, std::uint64_t count) {
        return wrap(take(stream).chunks(static_cast<std::size_t>(count)).map([](std::vector<Bytes> batch) { return pack_frames(batch); }));
    }
    ObjectPtr stream_chunks_timeout(const ObjectPtr &stream, std::uint64_t count, std::int64_t max_wait_ms) {
        return wrap(take(stream).chunks_timeout(static_cast<std::size_t>(count), ms(max_wait_ms)).map([](std::vector<Bytes> batch) { return pack_frames(batch); }));
    }
    ObjectPtr stream_chain(const ObjectPtr &first, const ObjectPtr &second) {
        auto a = take(first);
        return wrap(std::move(a).chain(take(second)));
    }
    ObjectPtr stream_merge(const ObjectPtr &first, const ObjectPtr &second) {
        auto a = take(first);
        return wrap(std::move(a).merge(take(second)));
    }
    ObjectPtr stream_zip(const ObjectPtr &first, const ObjectPtr &second) {
        auto a = take(first);
        return wrap(std::move(a).zip(take(second)).map([](std::pair<Bytes, Bytes> both) { return pack_frames({std::move(both.first), std::move(both.second)}); }));
    }
    ObjectPtr stream_map_concurrent(const ObjectPtr &stream, StreamMapFn fn, std::uint64_t concurrency, bool ordered) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        return wrap(take(stream).par_map([fn = std::move(fn)](Bytes item) { return fn(std::move(item)); }, static_cast<std::size_t>(concurrency), ordered));
    }
    ObjectPtr stream_throttle(const ObjectPtr &stream, std::int64_t period_ms) { return wrap(take(stream).throttle(ms(period_ms))); }
    ObjectPtr stream_timeout_each(const ObjectPtr &stream, std::int64_t limit_ms) { return wrap(take(stream).timeout_each(ms(limit_ms))); }

    // =================================================================================================================
    // Sinks
    // =================================================================================================================
    OpPtr stream_next(const ObjectPtr &stream) {
        auto &s = get<BytesStream>(stream, kStream);
        if (!s) throw BindingError(IoErrorCode::InvalidArgument, "the stream was already consumed");
        return launch([&s, stream](CancellationToken) { return s.next(); }, [](std::optional<Bytes> &&item, Value &out) {
            out.number = item ? 1 : 0;
            if (item) out.bytes = std::move(*item);
        });
    }
    OpPtr stream_collect(const ObjectPtr &stream) {
        auto s = take(stream);
        return launch([&](CancellationToken token) { return until_cancelled(std::move(s), std::move(token)).collect(); }, [](std::vector<Bytes> &&items, Value &out) {
            out.number = static_cast<std::int64_t>(items.size());
            out.bytes = pack_frames(items);
        });
    }
    OpPtr stream_for_each(const ObjectPtr &stream, StreamEachFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        auto s = take(stream);
        return launch([&](CancellationToken token) { return until_cancelled(std::move(s), std::move(token)).for_each([fn = std::move(fn)](Bytes item) { fn(item); }); }, [](Value &) {});
    }
    OpPtr stream_fold(const ObjectPtr &stream, Bytes initial, StreamFoldFn fn) {
        if (!fn) throw BindingError(IoErrorCode::InvalidArgument, "a function is required");
        auto s = take(stream);
        return launch(
            [&](CancellationToken token) {
                return until_cancelled(std::move(s), std::move(token)).fold(std::move(initial), [fn = std::move(fn)](Bytes accumulator, Bytes item) { return fn(std::move(accumulator), std::move(item)); });
            },
            [](Bytes &&accumulator, Value &out) { out.bytes = std::move(accumulator); });
    }
    OpPtr stream_count(const ObjectPtr &stream) {
        auto s = take(stream);
        return launch([&](CancellationToken token) { return until_cancelled(std::move(s), std::move(token)).count(); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr stream_last(const ObjectPtr &stream) {
        auto s = take(stream);
        return launch([&](CancellationToken token) { return until_cancelled(std::move(s), std::move(token)).last(); }, [](std::optional<Bytes> &&item, Value &out) {
            out.number = item ? 1 : 0;
            if (item) out.bytes = std::move(*item);
        });
    }
    OpPtr stream_forward(const ObjectPtr &stream, const ObjectPtr &sender) {
        auto s = take(stream);
        auto target = get<Sender<Bytes>>(sender, kChanSender);
        return launch([&](CancellationToken token) { return until_cancelled(std::move(s), std::move(token)).forward_to(target); }, [](Value &) {});
    }

    // =================================================================================================================
    // More data parallelism
    // =================================================================================================================
    std::uint32_t par_num_threads() { return static_cast<std::uint32_t>(current_num_threads()); }

    void par_join(const std::function<void()> &a, const std::function<void()> &b) { (void)SFT::Async::join([&] { a(); }, [&] { b(); }); }

    void par_scope(const std::function<void(const ObjectPtr &scope)> &body) {
        auto holder = make_object<Scope<> *>(kParScope, nullptr);
        auto &pointer = get<Scope<> *>(holder, kParScope);
        try {
            SFT::Async::scope([&](Scope<> &s) {
                pointer = &s;
                body(holder);
            });
        } catch (...) {
            pointer = nullptr;
            throw;
        }
        pointer = nullptr;
    }
    void par_scope_spawn(const ObjectPtr &scope, std::function<void()> fn) {
        Scope<> *pointer = get<Scope<> *>(scope, kParScope);
        if (pointer == nullptr) throw BindingError(IoErrorCode::Closed, "the scope has ended");
        pointer->spawn(std::move(fn));
    }

    void par_for_windows(std::uint64_t count, std::uint64_t window, const std::function<void(std::uint64_t)> &body) {
        if (window == 0) throw BindingError(IoErrorCode::InvalidArgument, "window must be positive");
        if (count < window) return;
        par_for(count - window + 1, 1, body);
    }

    bool par_any(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred) {
        return par_range(static_cast<usize>(count)).any([&](usize i) { return pred(i); });
    }
    bool par_all(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred) {
        return par_range(static_cast<usize>(count)).all([&](usize i) { return pred(i); });
    }
    std::uint64_t par_count(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred) {
        return par_range(static_cast<usize>(count)).filter([&](usize i) { return pred(i); }).count();
    }
    std::int64_t par_find_first(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred) {
        auto found = par_range(static_cast<usize>(count)).find_first([&](usize i) { return pred(i); });
        return found ? static_cast<std::int64_t>(*found) : -1;
    }
    std::int64_t par_find_any(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred) {
        auto found = par_range(static_cast<usize>(count)).find_any([&](usize i) { return pred(i); });
        return found ? static_cast<std::int64_t>(*found) : -1;
    }
    std::int64_t par_map_reduce_i64(std::uint64_t count, const std::function<std::int64_t(std::uint64_t)> &map, std::int64_t identity,
                                    const std::function<std::int64_t(std::int64_t, std::int64_t)> &reduce) {
        return par_range(static_cast<usize>(count)).map([&](usize i) -> std::int64_t { return map(i); }).reduce(identity, [&](std::int64_t a, std::int64_t b) { return reduce(a, b); });
    }
    double par_map_reduce_f64(std::uint64_t count, const std::function<double(std::uint64_t)> &map, double identity, const std::function<double(double, double)> &reduce) {
        return par_range(static_cast<usize>(count)).map([&](usize i) -> double { return map(i); }).reduce(identity, [&](double a, double b) { return reduce(a, b); });
    }

    void par_sort_by(void *data, std::uint64_t count, std::uint64_t element_size, bool stable, const std::function<bool(const void *, const void *)> &less) {
        if (count < 2) return;
        if (data == nullptr || element_size == 0) throw BindingError(IoErrorCode::InvalidArgument, "null data or zero element size");
        auto *bytes = static_cast<std::uint8_t *>(data);
        const std::size_t size = static_cast<std::size_t>(element_size);
        const Bytes copy(bytes, bytes + count * size);
        std::vector<std::size_t> order(static_cast<std::size_t>(count));
        std::iota(order.begin(), order.end(), std::size_t{0});
        auto compare = [&](std::size_t a, std::size_t b) { return less(copy.data() + a * size, copy.data() + b * size); };
        if (stable) SFT::Async::par_sort(order.begin(), order.end(), compare);
        else SFT::Async::par_sort_unstable(order.begin(), order.end(), compare);
        par_for(count, 1024, [&](std::uint64_t i) { std::memcpy(bytes + i * size, copy.data() + order[i] * size, size); });
    }

    namespace {
        template <typename T, typename Wide>
        bool reduce_typed(Reduction reduction, const void *data, std::uint64_t count, Wide &out) {
            std::span<const T> view(static_cast<const T *>(data), static_cast<std::size_t>(count));
            switch (reduction) {
                case Reduction::Sum: out = par_iter(view).map([](T v) { return static_cast<Wide>(v); }).sum(); return true;
                case Reduction::Product: out = par_iter(view).map([](T v) { return static_cast<Wide>(v); }).product(); return true;
                case Reduction::Min: {
                    auto v = par_iter(view).min();
                    if (!v) return false;
                    out = static_cast<Wide>(*v);
                    return true;
                }
                case Reduction::Max: {
                    auto v = par_iter(view).max();
                    if (!v) return false;
                    out = static_cast<Wide>(*v);
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool par_reduce(Numeric type, Reduction reduction, const void *data, std::uint64_t count, NumericValue *out) {
        if (out == nullptr) throw BindingError(IoErrorCode::InvalidArgument, "null output");
        if (data == nullptr && count != 0) throw BindingError(IoErrorCode::InvalidArgument, "null data");
        *out = NumericValue{};
        if (count == 0) {
            switch (reduction) {
                case Reduction::Sum: return true;
                case Reduction::Product:
                    out->f64 = 1;
                    out->i64 = 1;
                    out->u64 = 1;
                    return true;
                default: return false;
            }
        }
        switch (type) {
            case Numeric::F32: return reduce_typed<float, double>(reduction, data, count, out->f64);
            case Numeric::F64: return reduce_typed<double, double>(reduction, data, count, out->f64);
            case Numeric::I32: return reduce_typed<std::int32_t, std::int64_t>(reduction, data, count, out->i64);
            case Numeric::I64: return reduce_typed<std::int64_t, std::int64_t>(reduction, data, count, out->i64);
            case Numeric::U32: return reduce_typed<std::uint32_t, std::uint64_t>(reduction, data, count, out->u64);
            case Numeric::U64: return reduce_typed<std::uint64_t, std::uint64_t>(reduction, data, count, out->u64);
        }
        return false;
    }

} // namespace SFT::Async::Binding
