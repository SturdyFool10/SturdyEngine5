#include <Audio/Stream.hpp>
#include <Audio/Kernels.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cmath>
#include <thread>
#include <memory>
#include <vector>

namespace SFT::Audio {

    namespace {

        constexpr u32 kChunkFrames = 2048;

        struct Chunk {
            std::vector<f32> data; // interleaved, kChunkFrames * channels
            u32 frames = 0;
            u64 start = 0;         // source frame of the first frame
            u32 generation = 0;
            bool final = false;    // the stream ended with (or right after) this chunk
        };

    } // namespace

    struct StreamingSource::Impl {
        std::unique_ptr<StreamDecoder> decoder;
        AudioStreamInfo info;
        u32 output_rate = 48000;
        StreamingOptions options;
        bool loop = false;
        u64 loop_start = 0;
        u64 loop_end = 0; // 0 = the end of the stream

        // ---- chunk ring (producer: worker, consumer: audio thread)
        std::vector<Chunk> chunks;
        std::atomic<usize> write_index{0};
        std::atomic<usize> read_index{0};
        std::atomic<u32> seek_generation{0};
        std::atomic<u64> seek_target{0};
        std::atomic<bool> stop{false};
        // Decoder-side state, touched only by whichever pool worker is serving this stream (under `decode_mutex`).
        std::mutex decode_mutex;
        u32 worker_generation = 0;
        u64 decode_position = 0;
        bool eof = false;

        // ---- consumer state (audio thread only)
        u32 consumer_generation = 0;
        Chunk *current = nullptr;
        u32 current_consumed = 0;
        u64 next_pull_position = 0;
        std::vector<f32> window; // four native frames: [prev, a, b, next]
        bool primed = false;
        bool fast = false; // playing at the file's own rate: frames pass straight through, no interpolation window
        f64 phase = 0.0;
        f32 rate = 1.0f;
        bool ended = false;
        std::atomic<bool> finished_flag{false};
        std::atomic<bool> starved_flag{false};
        u64 pending_skip = 0;
        std::atomic<u64> published_position{0};

        bool pump();
        void request_seek(u64 frame);
        bool acquire_chunk();
        bool pull_frame(f32 *frame);
        bool prime();
    };

    // One step of the decode loop: serves a pending seek, then decodes at most one chunk if the ring has room. Returns whether it
    // did any work (so the pool knows when every stream is full and it can sleep).
    bool StreamingSource::Impl::pump() {
        std::scoped_lock lock(decode_mutex);
        const usize capacity = chunks.size();
        bool worked = false;
        const u32 wanted_generation = seek_generation.load(std::memory_order_acquire);
        if (wanted_generation != worker_generation) {
            const u64 target = seek_target.load(std::memory_order_acquire);
            decoder->seek(target);
            decode_position = target;
            worker_generation = wanted_generation;
            eof = false;
            worked = true;
        }
        if (eof) {
            return worked;
        }
        const usize w = write_index.load(std::memory_order_relaxed);
        const usize r = read_index.load(std::memory_order_acquire);
        if (w - r >= capacity) {
            return worked;
        }
        Chunk &chunk = chunks[w % capacity];
        chunk.generation = worker_generation;
        chunk.start = decode_position;
        chunk.final = false;

        u64 limit = kChunkFrames;
        const u64 region_end = loop_end > 0 ? loop_end : info.total_frames;
        if (loop && region_end > decode_position) {
            limit = std::min<u64>(limit, region_end - decode_position);
        }
        const u64 got = decoder->read(chunk.data.data(), limit);
        chunk.frames = static_cast<u32>(got);
        decode_position += got;

        bool publish = got > 0;
        if (loop && (got == 0 || (region_end > 0 && decode_position >= region_end))) {
            decoder->seek(loop_start);
            decode_position = loop_start;
            if (got == 0 && decode_position == chunk.start) {
                // An empty file region would spin forever; end the stream instead.
                chunk.final = true;
                publish = true;
                eof = true;
            }
        } else if (got == 0 && !loop) {
            chunk.final = true;
            publish = true;
            eof = true;
        }
        if (publish) {
            write_index.store(w + 1, std::memory_order_release);
        }
        return true;
    }

    void StreamingSource::Impl::request_seek(u64 frame) {
        seek_target.store(frame, std::memory_order_release);
        consumer_generation = seek_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        current = nullptr; // anything already queued is stale and is discarded as it surfaces
        current_consumed = 0;
        next_pull_position = frame;
        primed = false;
        phase = 0.0;
        ended = false;
        finished_flag.store(false, std::memory_order_relaxed);
        published_position.store(frame, std::memory_order_relaxed);
    }

    // Makes `current` point at a chunk with unread frames, advancing through the ring (and discarding chunks from before the
    // last seek). False when nothing is decoded yet, or the stream has ended (`ended` says which).
    bool StreamingSource::Impl::acquire_chunk() {
        for (;;) {
            if (current != nullptr && current_consumed < current->frames) {
                return true;
            }
            const usize capacity = chunks.size();
            if (current != nullptr) {
                const bool was_final = current->final;
                current = nullptr;
                read_index.store(read_index.load(std::memory_order_relaxed) + 1, std::memory_order_release);
                if (was_final) {
                    ended = true;
                    return false;
                }
            }
            const usize r = read_index.load(std::memory_order_relaxed);
            const usize w = write_index.load(std::memory_order_acquire);
            if (r == w) {
                return false; // nothing decoded yet
            }
            Chunk &candidate = chunks[r % capacity];
            if (candidate.generation != consumer_generation) {
                read_index.store(r + 1, std::memory_order_release); // stale: from before the last seek
                continue;
            }
            current = &candidate;
            current_consumed = 0;
            if (candidate.final && candidate.frames == 0) {
                ended = true;
                current = nullptr;
                read_index.store(r + 1, std::memory_order_release);
                return false;
            }
        }
    }

    bool StreamingSource::Impl::pull_frame(f32 *frame) {
        if (!acquire_chunk()) {
            return false;
        }
        std::copy_n(current->data.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(current_consumed) * info.channels), info.channels, frame);
        ++current_consumed;
        ++next_pull_position;
        return true;
    }

    bool StreamingSource::Impl::prime() {
        const u32 channels = info.channels;
        std::vector<f32> &w = window;
        // window = [first, first, second, third]: playback starts exactly at frame 0 of the region.
        if (!pull_frame(w.data() + static_cast<usize>(channels))) {
            return false;
        }
        std::copy_n(w.begin() + channels, channels, w.begin());
        for (u32 i = 2; i < 4; ++i) {
            if (!pull_frame(w.data() + static_cast<usize>(i) * channels)) {
                if (ended) {
                    // A very short stream: repeat the last frame to fill the window.
                    std::copy_n(w.begin() + static_cast<std::ptrdiff_t>((i - 1) * channels), channels, w.begin() + static_cast<std::ptrdiff_t>(i * channels));
                    continue;
                }
                return false;
            }
        }
        primed = true;
        return true;
    }


    // ---- the shared decode pool ------------------------------------------------------------------------------------------

    namespace {
        // A couple of threads serve every streaming source in the process, keeping the neediest rings topped up, so a game can
        // stream dozens of ambience layers and music stems without a thread (and a wake-up loop) apiece.
        class StreamPool {
          public:
            static StreamPool &instance() {
                static StreamPool pool;
                return pool;
            }
            void add(StreamingSource::Impl *impl) {
                std::scoped_lock lock(mutex_);
                streams_.push_back(impl);
            }
            void remove(StreamingSource::Impl *impl) {
                {
                    std::scoped_lock lock(mutex_);
                    std::erase(streams_, impl);
                }
                // A worker may be inside this stream right now; taking its lock waits for it to finish.
                std::scoped_lock wait(impl->decode_mutex);
            }

          private:
            StreamPool() {
                const u32 count = std::clamp(std::thread::hardware_concurrency() / 8, 1u, 3u);
                for (u32 i = 0; i < count; ++i) {
                    workers_.emplace_back([this] { run(); });
                }
            }
            ~StreamPool() {
                stop_.store(true);
                for (auto &w : workers_) w.join();
            }
            void run() {
                while (!stop_.load()) {
                    std::vector<StreamingSource::Impl *> snapshot;
                    {
                        std::scoped_lock lock(mutex_);
                        snapshot = streams_;
                    }
                    bool worked = false;
                    for (auto *impl : snapshot) {
                        // Stop serving a stream that has been removed between the snapshot and now.
                        {
                            std::scoped_lock lock(mutex_);
                            if (std::find(streams_.begin(), streams_.end(), impl) == streams_.end()) continue;
                        }
                        for (int i = 0; i < 4 && impl->pump(); ++i) worked = true;
                    }
                    if (!worked) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                }
            }

            std::mutex mutex_;
            std::vector<StreamingSource::Impl *> streams_;
            std::vector<std::thread> workers_;
            std::atomic<bool> stop_{false};
        };
    } // namespace

    // ---- public ---------------------------------------------------------------------------------------------------

    StreamingSource::StreamingSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    StreamingSource::~StreamingSource() {
        if (impl_) {
            impl_->stop.store(true, std::memory_order_release);
            StreamPool::instance().remove(impl_.get());
        }
    }

    std::expected<std::shared_ptr<StreamingSource>, UString> StreamingSource::from_decoder(std::unique_ptr<StreamDecoder> decoder, u32 output_rate,
                                                                                               const StreamingOptions &options) {
        if (!decoder) {
            return std::unexpected("audio: no decoder");
        }
        auto impl = std::make_unique<Impl>();
        impl->info = decoder->info();
        if (impl->info.channels == 0 || impl->info.sample_rate == 0) {
            return std::unexpected("audio: the stream has no channels or sample rate");
        }
        impl->output_rate = output_rate;
        impl->options = options;
        impl->loop = options.loop;
        if (options.loop && options.use_embedded_loop && impl->info.loop) {
            impl->loop_start = impl->info.loop->start;
            impl->loop_end = impl->info.loop->end;
        }
        const usize chunk_count = std::max<usize>(4, static_cast<usize>(std::ceil(options.buffer_seconds * static_cast<f32>(impl->info.sample_rate) / kChunkFrames)));
        impl->chunks.resize(chunk_count);
        for (Chunk &c : impl->chunks) {
            c.data.assign(static_cast<usize>(kChunkFrames) * impl->info.channels, 0.0f);
        }
        impl->window.assign(static_cast<usize>(4) * impl->info.channels, 0.0f);
        impl->decoder = std::move(decoder);
        StreamPool::instance().add(impl.get());
        return std::shared_ptr<StreamingSource>(new StreamingSource(std::move(impl)));
    }

    std::expected<std::shared_ptr<StreamingSource>, UString> StreamingSource::open_memory(EncodedBytes bytes, const UString &extension_hint,
                                                                                              u32 output_rate, const StreamingOptions &options) {
        auto decoder = DecoderRegistry::global().open(std::move(bytes), extension_hint);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        return from_decoder(std::move(*decoder), output_rate, options);
    }

    std::expected<std::shared_ptr<StreamingSource>, UString> StreamingSource::open(const std::filesystem::path &path, u32 output_rate,
                                                                                       const StreamingOptions &options) {
        auto bytes = read_file_bytes(path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return open_memory(*bytes, text_from_bytes(path.extension().string()), output_rate, options);
    }

    const AudioStreamInfo &StreamingSource::info() const noexcept { return impl_->info; }
    u32 StreamingSource::channel_count() const { return impl_->info.channels; }
    u32 StreamingSource::sample_rate() const { return impl_->output_rate; }
    u32 StreamingSource::native_rate() const { return impl_->info.sample_rate; }
    u64 StreamingSource::length_frames() const { return impl_->info.total_frames; }
    bool StreamingSource::finished() const { return impl_->finished_flag.load(std::memory_order_relaxed); }
    bool StreamingSource::starved() const noexcept { return impl_->starved_flag.load(std::memory_order_relaxed); }
    void StreamingSource::set_rate(f32 rate) { impl_->rate = rate > 0.0f ? rate : 0.0f; }

    u64 StreamingSource::position_frames() const { return impl_->published_position.load(std::memory_order_relaxed); }

    bool StreamingSource::seek_frames(u64 frame) {
        impl_->pending_skip = 0;
        impl_->request_seek(impl_->info.total_frames > 0 ? std::min(frame, impl_->info.total_frames) : frame);
        return true;
    }

    bool StreamingSource::skip(u64 frames) {
        // Virtual voices skip every block; the seek is deferred until the voice sounds again (decoding is not free).
        const f64 step = static_cast<f64>(impl_->rate) * impl_->info.sample_rate / impl_->output_rate;
        impl_->pending_skip += static_cast<u64>(static_cast<f64>(frames) * step);
        impl_->published_position.store(impl_->published_position.load(std::memory_order_relaxed) + static_cast<u64>(static_cast<f64>(frames) * step), std::memory_order_relaxed);
        return true;
    }

    u32 StreamingSource::read(AudioBuffer &out, u32 frames) {
        Impl &m = *impl_;
        if (m.finished_flag.load(std::memory_order_relaxed) || out.channels() == 0) {
            return 0;
        }
        if (m.pending_skip > 0) {
            const u64 target = m.published_position.load(std::memory_order_relaxed);
            m.pending_skip = 0;
            m.request_seek(m.info.total_frames > 0 ? std::min(target, m.info.total_frames) : target);
        }
        const u32 channels = m.info.channels;
        const u32 out_channels = std::min(channels, out.channels());
        const f64 step = static_cast<f64>(m.rate) * m.info.sample_rate / m.output_rate;
        m.starved_flag.store(false, std::memory_order_relaxed);

        u32 produced = 0;
        if (step == 1.0 && (m.fast || !m.primed || m.phase == 0.0)) {
            // The usual case: no pitch change and no rate conversion, so output is the decoded frames themselves.
            m.fast = true;
            m.primed = false;
            m.phase = 0.0;
            while (produced < frames) {
                if (!m.acquire_chunk()) {
                    if (m.ended) {
                        m.finished_flag.store(true, std::memory_order_relaxed);
                    } else {
                        m.starved_flag.store(true, std::memory_order_relaxed);
                        for (u32 c = 0; c < out_channels; ++c) {
                            std::fill(out.data(c) + produced, out.data(c) + frames, 0.0f);
                        }
                        produced = frames;
                    }
                    break;
                }
                const u32 n = std::min(frames - produced, m.current->frames - m.current_consumed);
                const f32 *src = m.current->data.data() + static_cast<usize>(m.current_consumed) * channels;
                for (u32 c = 0; c < out_channels; ++c) {
                    f32 *dst = out.data(c) + produced;
                    for (u32 i = 0; i < n; ++i) {
                        dst[i] = src[static_cast<usize>(i) * channels + c];
                    }
                }
                m.current_consumed += n;
                m.next_pull_position += n;
                produced += n;
            }
            m.published_position.store(m.next_pull_position, std::memory_order_relaxed);
            return produced;
        }
        m.fast = false;
        for (; produced < frames; ++produced) {
            if (!m.primed && !m.prime()) {
                if (m.ended) {
                    m.finished_flag.store(true, std::memory_order_relaxed);
                } else {
                    m.starved_flag.store(true, std::memory_order_relaxed);
                    // Hold the position and give silence for the rest of this block.
                    for (u32 c = 0; c < out_channels; ++c) {
                        std::fill(out.data(c) + produced, out.data(c) + frames, 0.0f);
                    }
                    return frames;
                }
                break;
            }
            while (m.phase >= 1.0) {
                std::copy(m.window.begin() + channels, m.window.end(), m.window.begin());
                if (!m.pull_frame(m.window.data() + static_cast<usize>(3) * channels)) {
                    if (m.ended) {
                        // Interpolate out against the last frame, then finish when the window is exhausted.
                        std::copy_n(m.window.begin() + static_cast<std::ptrdiff_t>(2 * channels), channels, m.window.begin() + static_cast<std::ptrdiff_t>(3 * channels));
                        m.finished_flag.store(true, std::memory_order_relaxed);
                    } else {
                        // Starved: undo the shift by repeating the newest frame, and wait for data.
                        std::copy_n(m.window.begin() + static_cast<std::ptrdiff_t>(2 * channels), channels, m.window.begin() + static_cast<std::ptrdiff_t>(3 * channels));
                        m.starved_flag.store(true, std::memory_order_relaxed);
                        for (u32 c = 0; c < out_channels; ++c) {
                            std::fill(out.data(c) + produced, out.data(c) + frames, 0.0f);
                        }
                        m.phase = 0.0;
                        return frames;
                    }
                }
                m.phase -= 1.0;
            }
            const f32 t = static_cast<f32>(m.phase);
            for (u32 c = 0; c < out_channels; ++c) {
                const f32 p0 = m.window[c], p1 = m.window[channels + c], p2 = m.window[2 * channels + c], p3 = m.window[3 * channels + c];
                out.data(c)[produced] = Kernels::catmull_rom(p0, p1, p2, p3, t);
            }
            m.phase += step;
            if (m.finished_flag.load(std::memory_order_relaxed)) {
                ++produced;
                break;
            }
        }
        // The window holds two frames of lookahead beyond the playback point.
        const u64 ahead = m.next_pull_position >= 2 ? m.next_pull_position - 2 : 0;
        m.published_position.store(ahead, std::memory_order_relaxed);
        return produced;
    }

} // namespace SFT::Audio
