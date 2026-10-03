#include <Audio/Source.hpp>

#include <Audio/Kernels.hpp>

#include <Async/SpscRingBuffer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace SFT::Audio {

    // ---- BufferSource ----------------------------------------------------------------------------------------

    BufferSource::BufferSource(std::shared_ptr<const SampleBuffer> buffer, u32 output_rate, bool loop)
        : buffer_(std::move(buffer)), output_rate_(output_rate), loop_(loop) {
        finished_ = !buffer_ || buffer_->frames() == 0;
    }

    f32 BufferSource::sample_at(i64 frame, u32 channel) const noexcept {
        const i64 frames = static_cast<i64>(buffer_->frames());
        if (loop_) {
            frame %= frames;
            if (frame < 0) {
                frame += frames;
            }
        } else if (frame < 0 || frame >= frames) {
            return 0.0f;
        }
        return (*buffer_->samples)[static_cast<usize>(frame) * buffer_->channels + channel];
    }

    u32 BufferSource::read(AudioBuffer &out, u32 frames) {
        if (finished_) {
            return 0;
        }
        const i64 length = static_cast<i64>(buffer_->frames());
        const f64 step = static_cast<f64>(rate_) * static_cast<f64>(buffer_->sample_rate) / static_cast<f64>(output_rate_);
        const u32 channels = std::min(buffer_->channels, out.channels());
        const u32 stride = buffer_->channels;
        const f32 *base = buffer_->samples->data();
        u32 produced = 0;

        // The common case by far: the sound plays at its own rate from a whole-frame position. There is nothing to
        // interpolate, so copy straight out of the interleaved data (wrapping for loops).
        if (std::fabs(step - 1.0) < 1e-9 && position_ == std::floor(position_)) {
            i64 position = static_cast<i64>(position_);
            while (produced < frames) {
                if (position >= length) {
                    if (!loop_) {
                        finished_ = true;
                        break;
                    }
                    position %= length;
                }
                const u32 n = static_cast<u32>(std::min<i64>(frames - produced, length - position));
                out.load_interleaved(std::span<const f32>{base + static_cast<usize>(position) * stride, static_cast<usize>(n) * stride}, stride, produced);
                position += n;
                produced += n;
            }
            position_ = static_cast<f64>(position);
            if (loop_ && position_ >= static_cast<f64>(length)) {
                position_ = std::fmod(position_, static_cast<f64>(length));
            }
            return produced;
        }

        const f64 length_f = static_cast<f64>(length);
        for (; produced < frames; ++produced) {
            if (!loop_ && position_ >= length_f) {
                finished_ = true;
                break;
            }
            const i64 i1 = static_cast<i64>(position_); // position_ is never negative, so this is the floor
            const f32 t = static_cast<f32>(position_ - static_cast<f64>(i1));
            if (i1 >= 1 && i1 + 2 < length) {
                // Interior: all four neighbours are in range, so index the data directly.
                const f32 *p = base + static_cast<usize>(i1 - 1) * stride;
                for (u32 c = 0; c < channels; ++c) {
                    // Catmull-Rom cubic through the four neighbouring frames.
                    const f32 p0 = p[c], p1 = p[stride + c], p2 = p[2 * static_cast<usize>(stride) + c], p3 = p[3 * static_cast<usize>(stride) + c];
                    out.data(c)[produced] = Kernels::catmull_rom(p0, p1, p2, p3, t);
                }
            } else {
                for (u32 c = 0; c < channels; ++c) {
                    const f32 p0 = sample_at(i1 - 1, c), p1 = sample_at(i1, c), p2 = sample_at(i1 + 1, c), p3 = sample_at(i1 + 2, c);
                    out.data(c)[produced] = Kernels::catmull_rom(p0, p1, p2, p3, t);
                }
            }
            position_ += step;
            if (loop_ && position_ >= length_f) {
                position_ -= length_f;
                if (position_ >= length_f) {
                    position_ = std::fmod(position_, length_f);
                }
            }
        }
        return produced;
    }

    BufferSource::BlockPlan BufferSource::plan_block(u32 frames) {
        BlockPlan plan;
        plan.loop = loop_;
        if (finished_ || frames == 0) {
            return plan;
        }
        const f64 length = static_cast<f64>(buffer_->frames());
        const f64 step = static_cast<f64>(rate_) * static_cast<f64>(buffer_->sample_rate) / static_cast<f64>(output_rate_);
        const f64 whole = std::floor(position_);
        plan.base = static_cast<i64>(whole);
        plan.frac = static_cast<f32>(position_ - whole);
        plan.step = static_cast<f32>(step);
        plan.valid_frames = frames;
        if (!loop_ && position_ + step * static_cast<f64>(frames - 1) >= length) {
            // The sound ends inside this block: count the frames that start before the end.
            const f64 remaining = length - position_;
            const f64 count = step > 0.0 ? std::ceil(remaining / step) : 0.0;
            plan.valid_frames = static_cast<u32>(std::clamp(count, 0.0, static_cast<f64>(frames)));
            finished_ = true;
        }
        position_ += step * static_cast<f64>(plan.valid_frames);
        if (loop_ && position_ >= length) {
            position_ = std::fmod(position_, length);
        }
        return plan;
    }

    bool BufferSource::skip(u64 frames) {
        const f64 step = static_cast<f64>(rate_) * static_cast<f64>(buffer_->sample_rate) / static_cast<f64>(output_rate_);
        position_ += step * static_cast<f64>(frames);
        const f64 length = static_cast<f64>(buffer_->frames());
        if (length <= 0.0) {
            finished_ = true;
        } else if (loop_) {
            position_ = std::fmod(position_, length);
        } else if (position_ >= length) {
            finished_ = true;
        }
        return true;
    }

    bool BufferSource::seek_frames(u64 frame) {
        position_ = static_cast<f64>(frame);
        finished_ = !loop_ && frame >= buffer_->frames();
        return true;
    }

    void BufferSource::seek_seconds(f64 seconds) noexcept {
        position_ = std::max(0.0, seconds * static_cast<f64>(buffer_->sample_rate));
        finished_ = !loop_ && position_ >= static_cast<f64>(buffer_->frames());
    }

    f64 BufferSource::position_seconds() const noexcept {
        return position_ / static_cast<f64>(buffer_->sample_rate);
    }

    // ---- RateConverter -----------------------------------------------------------------------------------------

    RateConverter::RateConverter(std::shared_ptr<DataSource> inner, u32 max_block_frames) : inner_(std::move(inner)) {
        // Room for the largest block at the largest supported rate (8x) plus the interpolation history.
        input_.resize(std::max(inner_->channel_count(), 1u), max_block_frames * 8 + 16);
        scratch_.resize(input_.channels(), input_.frames());
    }

    bool RateConverter::refill() {
        if (ended_) {
            return false;
        }
        const u32 capacity = input_.frames();
        // Keep the interpolation history (one frame before the read position) and everything after it.
        const u64 keep_from = input_position_ >= 1.0 ? static_cast<u64>(input_position_) - 1 : 0;
        const u64 keep = input_length_ > keep_from ? input_length_ - keep_from : 0;
        if (keep > 0 && keep_from > 0) {
            for (u32 c = 0; c < input_.channels(); ++c) {
                std::copy(input_.data(c) + keep_from, input_.data(c) + keep_from + keep, input_.data(c));
            }
        }
        input_position_ -= static_cast<f64>(keep_from);
        input_length_ = keep;

        // Read the rest of the window from the source into a scratch block and append it.
        const u32 wanted = capacity - static_cast<u32>(input_length_);
        const u32 produced = inner_->read(scratch_, wanted);
        for (u32 c = 0; c < input_.channels(); ++c) {
            std::copy_n(scratch_.data(c), produced, input_.data(c) + input_length_);
        }
        input_length_ += produced;
        if (produced < wanted) {
            ended_ = true;
        }
        return produced > 0;
    }

    u32 RateConverter::read(AudioBuffer &out, u32 frames) {
        const u32 channels = std::min(out.channels(), input_.channels());
        u32 produced = 0;
        for (; produced < frames; ++produced) {
            const u64 base = static_cast<u64>(std::floor(input_position_));
            if (base + 3 > input_length_) {
                if (!refill()) {
                    // Drain: the last frames interpolate against silence past the end.
                    if (static_cast<u64>(std::floor(input_position_)) + 1 >= input_length_) {
                        break;
                    }
                }
            }
            const i64 i1 = static_cast<i64>(std::floor(input_position_));
            const f32 t = static_cast<f32>(input_position_ - static_cast<f64>(i1));
            const auto at = [&](i64 index, u32 channel) -> f32 {
                return index >= 0 && static_cast<u64>(index) < input_length_ ? input_.data(channel)[index] : 0.0f;
            };
            for (u32 c = 0; c < channels; ++c) {
                const f32 p0 = at(i1 - 1, c), p1 = at(i1, c), p2 = at(i1 + 1, c), p3 = at(i1 + 2, c);
                out.data(c)[produced] = Kernels::catmull_rom(p0, p1, p2, p3, t);
            }
            input_position_ += rate_;
        }
        return produced;
    }

    bool RateConverter::skip(u64 frames) {
        // Advance through the input without interpolating.
        input_position_ += static_cast<f64>(frames) * rate_;
        while (static_cast<u64>(std::max(input_position_, 0.0)) + 3 > input_length_) {
            if (!refill()) {
                break;
            }
        }
        return true;
    }

    bool RateConverter::seek_frames(u64 frame) {
        if (!inner_->seek_frames(frame)) {
            return false;
        }
        input_length_ = 0;
        input_position_ = 0.0;
        ended_ = false;
        return true;
    }

    u64 RateConverter::position_frames() const {
        // The inner source has read ahead by whatever is still buffered after our read position.
        const u64 inner = inner_->position_frames();
        const u64 buffered = input_length_ > static_cast<u64>(input_position_) ? input_length_ - static_cast<u64>(input_position_) : 0;
        return inner > buffered ? inner - buffered : 0;
    }

    // ---- LiveSource -------------------------------------------------------------------------------------------------

    namespace {
        usize round_up_pow2(usize n) {
            usize p = 64;
            while (p < n) {
                p <<= 1;
            }
            return p;
        }
    } // namespace

    struct LiveSource::Impl {
        Impl(u32 channels, usize capacity_frames, const LiveSourceOptions &o)
            : capacity(round_up_pow2(capacity_frames)), mask(capacity - 1), samples(capacity * channels, 0.0f), options(o), channel_count(channels) {}

        const usize capacity;     // frames, a power of two
        const usize mask;
        std::vector<f32> samples; // interleaved ring
        // Producer writes `head`, consumer writes `tail`; both count frames forever (wrapping is by mask).
        alignas(64) std::atomic<u64> head{0};
        alignas(64) std::atomic<u64> tail{0};
        std::atomic<bool> closed{false};
        std::atomic<u64> underruns{0}, dropped{0}, overflowed{0};
        std::atomic<f64> adjustment{0.0};
        LiveSourceOptions options;
        u32 channel_count;
        ChannelLayoutInfo layout;

        // Consumer-only state.
        bool started = false;
        f64 phase = 0.0;      // fractional read position past `tail`
        f32 smoothed_adjustment = 0.0f;
        bool ever_played = false;

        [[nodiscard]] f32 sample(u64 frame, u32 channel) const noexcept { return samples[(frame & mask) * channel_count + channel]; }
    };

    LiveSource::LiveSource(u32 channels, u32 input_rate, u32 output_rate, usize capacity_frames, const LiveSourceOptions &options)
        : impl_(std::make_unique<Impl>(std::max(channels, 1u), std::max<usize>(capacity_frames, 64), options)), channels_(std::max(channels, 1u)),
          input_rate_(std::max(input_rate, 1u)), output_rate_(std::max(output_rate, 1u)) {
        impl_->layout = channels_ <= 2 ? ChannelLayoutInfo::guess(channels_) : ChannelLayoutInfo::discrete(channels_);
    }

    LiveSource::LiveSource(u32 sample_rate, usize capacity_frames) : LiveSource(1, sample_rate, sample_rate, capacity_frames, LiveSourceOptions{.adaptive = false}) {}

    LiveSource::~LiveSource() = default;

    void LiveSource::set_layout(const ChannelLayoutInfo &layout) {
        if (layout.channels == channels_) {
            impl_->layout = layout;
        }
    }

    ChannelLayoutInfo LiveSource::channel_layout() const { return impl_->layout; }

    usize LiveSource::push(const f32 *interleaved, usize frames) noexcept {
        Impl &m = *impl_;
        const u64 head = m.head.load(std::memory_order_relaxed);
        const u64 tail = m.tail.load(std::memory_order_acquire);
        const usize free_frames = m.capacity - static_cast<usize>(head - tail);
        const usize n = std::min(frames, free_frames);
        if (n < frames) {
            m.overflowed.fetch_add(frames - n, std::memory_order_relaxed);
        }
        usize done = 0;
        while (done < n) {
            const usize at = static_cast<usize>((head + done) & m.mask);
            const usize run = std::min(n - done, m.capacity - at);
            std::copy_n(interleaved + done * channels_, run * channels_, m.samples.begin() + static_cast<std::ptrdiff_t>(at * channels_));
            done += run;
        }
        m.head.store(head + n, std::memory_order_release);
        return n;
    }

    usize LiveSource::push_planar(const f32 *const *planes, usize frames) noexcept {
        Impl &m = *impl_;
        const u64 head = m.head.load(std::memory_order_relaxed);
        const u64 tail = m.tail.load(std::memory_order_acquire);
        const usize n = std::min(frames, m.capacity - static_cast<usize>(head - tail));
        if (n < frames) {
            m.overflowed.fetch_add(frames - n, std::memory_order_relaxed);
        }
        for (usize i = 0; i < n; ++i) {
            f32 *dst = m.samples.data() + static_cast<usize>((head + i) & m.mask) * channels_;
            for (u32 c = 0; c < channels_; ++c) {
                dst[c] = planes[c][i];
            }
        }
        m.head.store(head + n, std::memory_order_release);
        return n;
    }

    void LiveSource::close() noexcept { impl_->closed.store(true, std::memory_order_release); }

    u64 LiveSource::buffered_frames() const noexcept { return impl_->head.load(std::memory_order_acquire) - impl_->tail.load(std::memory_order_acquire); }
    f64 LiveSource::buffered_seconds() const noexcept { return static_cast<f64>(buffered_frames()) / static_cast<f64>(input_rate_); }
    u64 LiveSource::underruns() const noexcept { return impl_->underruns.load(std::memory_order_relaxed); }
    u64 LiveSource::dropped_frames() const noexcept { return impl_->dropped.load(std::memory_order_relaxed); }
    u64 LiveSource::overflowed_frames() const noexcept { return impl_->overflowed.load(std::memory_order_relaxed); }
    f64 LiveSource::rate_adjustment() const noexcept { return impl_->adjustment.load(std::memory_order_relaxed); }

    u32 LiveSource::read(AudioBuffer &out, u32 frames) {
        Impl &m = *impl_;
        const u32 channels = std::min(channels_, out.channels());
        const u64 head = m.head.load(std::memory_order_acquire);
        u64 tail = m.tail.load(std::memory_order_relaxed);
        u64 fill = head - tail;

        if (!m.started) {
            // Collect the cushion first, so the first block does not immediately underrun.
            if (fill < std::max<u32>(m.options.prefill_frames, 1)) {
                return 0;
            }
            m.started = true;
        }
        // A backlog beyond the limit means the consumer stalled; drop the oldest audio rather than play it late.
        if (m.options.max_latency_frames > 0 && fill > m.options.max_latency_frames) {
            const u64 keep = std::max<u32>(m.options.prefill_frames, 1);
            const u64 drop = fill - keep;
            tail += drop;
            m.tail.store(tail, std::memory_order_release);
            m.dropped.fetch_add(drop, std::memory_order_relaxed);
            fill = keep;
            m.phase = 0.0;
        }

        // Steer the read rate to keep the backlog at the cushion: a fuller ring plays slightly faster.
        f64 drift = 0.0;
        if (m.options.adaptive && m.options.prefill_frames > 0) {
            const f64 target = static_cast<f64>(m.options.prefill_frames);
            const f64 relative = (static_cast<f64>(fill) - target) / target;
            const f32 wanted = static_cast<f32>(std::clamp(relative * 0.01, -static_cast<f64>(m.options.max_rate_adjustment), static_cast<f64>(m.options.max_rate_adjustment)));
            m.smoothed_adjustment += (wanted - m.smoothed_adjustment) * 0.05f;
            drift = static_cast<f64>(m.smoothed_adjustment);
            m.adjustment.store(drift, std::memory_order_relaxed);
        }
        const f64 step = static_cast<f64>(rate_) * (1.0 + drift) * static_cast<f64>(input_rate_) / static_cast<f64>(output_rate_);

        u32 produced = 0;
        const bool unity = std::fabs(step - 1.0) < 1e-9 && m.phase == 0.0;
        if (unity) {
            // Straight copy: no interpolation needed.
            const u32 n = static_cast<u32>(std::min<u64>(frames, fill));
            for (u32 i = 0; i < n; ++i) {
                const f32 *src = m.samples.data() + static_cast<usize>((tail + i) & m.mask) * channels_;
                for (u32 c = 0; c < channels; ++c) {
                    out.data(c)[i] = src[c];
                }
            }
            tail += n;
            produced = n;
        } else {
            // Catmull-Rom between the frames around the read position; needs two frames of look-ahead.
            u64 position = tail;
            while (produced < frames) {
                if (position + 3 > head) {
                    break;
                }
                const f32 t = static_cast<f32>(m.phase);
                const u64 i1 = position;
                for (u32 c = 0; c < channels; ++c) {
                    const f32 p0 = m.sample(i1 > 0 ? i1 - 1 : 0, c), p1 = m.sample(i1, c), p2 = m.sample(i1 + 1, c), p3 = m.sample(i1 + 2, c);
                    out.data(c)[produced] = Kernels::catmull_rom(p0, p1, p2, p3, t);
                }
                m.phase += step;
                const u64 advance = static_cast<u64>(m.phase);
                position += advance;
                m.phase -= static_cast<f64>(advance);
                ++produced;
            }
            tail = position;
        }
        m.tail.store(tail, std::memory_order_release);
        if (produced < frames) {
            // Out of audio mid-block. The caller pads the rest with silence; once a stream has begun that is an underrun.
            if (m.started && !(m.closed.load(std::memory_order_acquire) && head == tail)) {
                m.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return produced;
    }

    bool LiveSource::finished() const {
        return impl_->closed.load(std::memory_order_acquire) && buffered_frames() == 0;
    }

} // namespace SFT::Audio
