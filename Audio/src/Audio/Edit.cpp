#include <Audio/Edit.hpp>

#include <Audio/Interleaved.hpp>
#include <Foundation/Iter.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Loudness.hpp>
#include <Audio/Resample.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace SFT::Audio::edit {

    namespace {

        usize ch(const SampleBuffer &b) { return b.channels; }

        /// A buffer with `like`'s format, markers and loop, holding `samples`.
        Buffer wrap(const SampleBuffer &like, std::vector<f32> samples) {
            auto out = std::make_shared<SampleBuffer>();
            out->channels = like.channels;
            out->sample_rate = like.sample_rate;
            out->layout = like.layout;
            out->markers = like.markers;
            out->loop = like.loop;
            out->samples = std::make_shared<const std::vector<f32>>(std::move(samples));
            return out;
        }

        const std::vector<f32> &data(const SampleBuffer &b) {
            static const std::vector<f32> empty;
            return b.samples ? *b.samples : empty;
        }

        f32 curve_value(FadeCurve curve, f32 t) {
            t = std::clamp(t, 0.0f, 1.0f);
            switch (curve) {
                case FadeCurve::Linear: return t;
                case FadeCurve::EqualPower: return std::sin(t * std::numbers::pi_v<f32> * 0.5f);
                case FadeCurve::Exponential: return t * t;
                case FadeCurve::Logarithmic: return 1.0f - (1.0f - t) * (1.0f - t);
                case FadeCurve::SCurve: return t * t * (3.0f - 2.0f * t);
            }
            return t;
        }

        /// Converts `piece` to `like`'s channel count and rate.
        Buffer match_format(const SampleBuffer &piece, const SampleBuffer &like) {
            Buffer current = std::make_shared<SampleBuffer>(piece);
            if (current->channels != like.channels) {
                current = convert_channels(*current, like.channels);
            }
            if (current->sample_rate != like.sample_rate) {
                current = convert_rate(*current, like.sample_rate);
            }
            return current;
        }

        // Marker bookkeeping for structural edits.
        void clip_markers(SampleBuffer &b, u64 start, u64 end) {
            std::vector<AudioMarker> kept;
            for (const AudioMarker &m : b.markers) {
                if (m.frame >= start && m.frame < end) kept.push_back(AudioMarker{m.name, m.frame - start});
            }
            b.markers = std::move(kept);
            if (b.loop && b.loop->start >= start && b.loop->end <= end) {
                b.loop = LoopRegion{b.loop->start - start, b.loop->end - start};
            } else {
                b.loop.reset();
            }
        }

    } // namespace

    u64 frames_at(const SampleBuffer &buffer, f64 seconds) noexcept {
        return static_cast<u64>(std::max(0.0, std::round(seconds * static_cast<f64>(buffer.sample_rate))));
    }
    f64 seconds_at(const SampleBuffer &buffer, u64 frames) noexcept {
        return buffer.sample_rate > 0 ? static_cast<f64>(frames) / static_cast<f64>(buffer.sample_rate) : 0.0;
    }

    // ---- structure ---------------------------------------------------------------------------------------------------------

    Buffer slice(const SampleBuffer &b, u64 start, u64 end) {
        const u64 frames = b.frames();
        end = std::min(end, frames);
        start = std::min(start, end);
        const auto &d = data(b);
        Buffer out = wrap(b, std::vector<f32>(d.begin() + static_cast<std::ptrdiff_t>(start * ch(b)), d.begin() + static_cast<std::ptrdiff_t>(end * ch(b))));
        clip_markers(*out, start, end);
        return out;
    }

    Buffer remove(const SampleBuffer &b, u64 start, u64 end) {
        const u64 frames = b.frames();
        end = std::min(end, frames);
        start = std::min(start, end);
        const auto &d = data(b);
        std::vector<f32> samples;
        samples.reserve(d.size() - (end - start) * ch(b));
        samples.insert(samples.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(start * ch(b)));
        samples.insert(samples.end(), d.begin() + static_cast<std::ptrdiff_t>(end * ch(b)), d.end());
        Buffer out = wrap(b, std::move(samples));
        const u64 cut = end - start;
        std::vector<AudioMarker> kept;
        for (const AudioMarker &m : b.markers) {
            if (m.frame < start) kept.push_back(m);
            else if (m.frame >= end) kept.push_back(AudioMarker{m.name, m.frame - cut});
        }
        out->markers = std::move(kept);
        if (b.loop) {
            if (b.loop->end <= start) out->loop = b.loop;
            else if (b.loop->start >= end) out->loop = LoopRegion{b.loop->start - cut, b.loop->end - cut};
            else out->loop.reset();
        }
        return out;
    }

    Buffer insert(const SampleBuffer &b, u64 at, const SampleBuffer &piece) {
        const Buffer fitted = match_format(piece, b);
        at = std::min(at, b.frames());
        const auto &d = data(b);
        const auto &p = data(*fitted);
        std::vector<f32> samples;
        samples.reserve(d.size() + p.size());
        samples.insert(samples.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(at * ch(b)));
        samples.insert(samples.end(), p.begin(), p.end());
        samples.insert(samples.end(), d.begin() + static_cast<std::ptrdiff_t>(at * ch(b)), d.end());
        Buffer out = wrap(b, std::move(samples));
        const u64 added = fitted->frames();
        for (AudioMarker &m : out->markers) {
            if (m.frame >= at) m.frame += added;
        }
        if (out->loop) {
            if (out->loop->start >= at) out->loop = LoopRegion{out->loop->start + added, out->loop->end + added};
            else if (out->loop->end > at) out->loop->end += added;
        }
        return out;
    }

    Result concat(std::span<const SampleBuffer *const> buffers, u64 crossfade_frames, FadeCurve curve) {
        if (buffers.empty() || buffers.front() == nullptr) {
            return std::unexpected("audio: nothing to join");
        }
        const SampleBuffer &first = *buffers.front();
        Buffer result = std::make_shared<SampleBuffer>(first);
        for (usize i = 1; i < buffers.size(); ++i) {
            const Buffer next = match_format(*buffers[i], first);
            const u64 overlap = std::min({crossfade_frames, result->frames(), next->frames()});
            const auto &a = data(*result);
            const auto &b = data(*next);
            std::vector<f32> samples(a.begin(), a.end());
            const usize c = first.channels;
            // The overlapping stretch: the old audio fades out while the new fades in.
            const usize base = (result->frames() - overlap) * c;
            const auto old_tail = frames_of(std::span<f32>{samples}.subspan(base), c);
            const auto new_head = frames_of(std::span<const f32>{b}.first(overlap * c), c);
            for (const auto [f, mine, theirs] : std::views::zip(std::views::iota(u64{0}), old_tail, new_head)) {
                const f32 t = (static_cast<f32>(f) + 0.5f) / static_cast<f32>(overlap);
                const f32 gain_out = curve_value(curve, 1.0f - t), gain_in = curve_value(curve, t);
                for (const auto [out, in] : std::views::zip(mine, theirs)) {
                    out = out * gain_out + in * gain_in;
                }
            }
            samples.insert(samples.end(), b.begin() + static_cast<std::ptrdiff_t>(overlap * c), b.end());
            const u64 shift = result->frames() - overlap;
            Buffer joined = wrap(*result, std::move(samples));
            for (const AudioMarker &m : next->markers) {
                joined->markers.push_back(AudioMarker{m.name, m.frame + shift});
            }
            result = std::move(joined);
        }
        return result;
    }

    Buffer repeat(const SampleBuffer &b, u32 times) {
        times = std::max(times, 1u);
        const auto &d = data(b);
        std::vector<f32> samples;
        samples.reserve(d.size() * times);
        for (u32 i = 0; i < times; ++i) samples.insert(samples.end(), d.begin(), d.end());
        Buffer out = wrap(b, std::move(samples));
        out->markers.clear();
        for (u32 i = 0; i < times; ++i) {
            for (const AudioMarker &m : b.markers) out->markers.push_back(AudioMarker{m.name, m.frame + static_cast<u64>(i) * b.frames()});
        }
        out->loop.reset();
        return out;
    }

    Buffer reverse(const SampleBuffer &b) {
        const auto &d = data(b);
        const usize frames = static_cast<usize>(b.frames());
        std::vector<f32> samples;
        samples.reserve(d.size());
        for (const auto frame : frames_of(std::span<const f32>{d}, ch(b)) | std::views::reverse) {
            samples.insert(samples.end(), frame.begin(), frame.end());
        }
        Buffer out = wrap(b, std::move(samples));
        for (AudioMarker &m : out->markers) m.frame = frames > m.frame ? frames - 1 - m.frame : 0;
        std::sort(out->markers.begin(), out->markers.end(), [](const AudioMarker &x, const AudioMarker &y) { return x.frame < y.frame; });
        if (out->loop) out->loop = LoopRegion{frames - out->loop->end, frames - out->loop->start};
        return out;
    }

    Buffer silence(const SampleBuffer &like, u64 frames) {
        auto out = wrap(like, std::vector<f32>(static_cast<usize>(frames) * like.channels, 0.0f));
        out->markers.clear();
        out->loop.reset();
        return out;
    }

    // ---- level and fades ------------------------------------------------------------------------------------------------------

    Buffer gain(const SampleBuffer &b, f32 decibels) {
        std::vector<f32> samples = data(b);
        Kernels::scale(samples, std::pow(10.0f, decibels / 20.0f));
        return wrap(b, std::move(samples));
    }

    Buffer fade(const SampleBuffer &b, u64 start, u64 end, f32 from_gain, f32 to_gain, FadeCurve curve) {
        std::vector<f32> samples = data(b);
        const usize c = ch(b), frames = static_cast<usize>(b.frames());
        end = std::min<u64>(end, frames);
        start = std::min(start, end);
        const f32 length = static_cast<f32>(std::max<u64>(end - start, 1));
        for (const auto [f, frame] : indexed_frames(std::span<f32>{samples}, c)) {
            f32 g;
            if (f < start) g = from_gain;
            else if (f >= end) g = to_gain;
            else g = from_gain + (to_gain - from_gain) * curve_value(curve, (static_cast<f32>(f - start) + 0.5f) / length);
            for (f32 &sample : frame) sample *= g;
        }
        return wrap(b, std::move(samples));
    }

    Buffer fade_in(const SampleBuffer &b, u64 frames, FadeCurve curve) { return fade(b, 0, frames, 0.0f, 1.0f, curve); }
    Buffer fade_out(const SampleBuffer &b, u64 frames, FadeCurve curve) {
        const u64 total = b.frames();
        return fade(b, total > frames ? total - frames : 0, total, 1.0f, 0.0f, curve);
    }

    Buffer gain_envelope(const SampleBuffer &b, std::span<const std::pair<u64, f32>> points) {
        if (points.empty()) {
            return std::make_shared<SampleBuffer>(b);
        }
        std::vector<f32> samples = data(b);
        const usize c = ch(b);
        usize segment = 0;
        for (const auto [f, frame] : indexed_frames(std::span<f32>{samples}, c)) {
            while (segment + 1 < points.size() && f >= points[segment + 1].first) ++segment;
            f32 g;
            if (f < points.front().first) g = points.front().second;
            else if (segment + 1 >= points.size()) g = points.back().second;
            else {
                const auto &p0 = points[segment], &p1 = points[segment + 1];
                const f32 t = static_cast<f32>(f - p0.first) / static_cast<f32>(std::max<u64>(p1.first - p0.first, 1));
                g = p0.second + (p1.second - p0.second) * t;
            }
            for (f32 &sample : frame) sample *= g;
        }
        return wrap(b, std::move(samples));
    }

    Buffer mix(const SampleBuffer &a, const SampleBuffer &b, u64 offset, f32 gain_a, f32 gain_b) {
        const Buffer fitted = match_format(b, a);
        const usize c = ch(a);
        const u64 length = std::max<u64>(a.frames(), offset + fitted->frames());
        std::vector<f32> samples(static_cast<usize>(length) * c, 0.0f);
        const auto &da = data(a);
        const auto &db = data(*fitted);
        for (const auto [out, in] : std::views::zip(samples, da)) out = in * gain_a;
        for (const auto [out, in] : std::views::zip(std::span<f32>{samples}.subspan(static_cast<usize>(offset) * c), db)) out += in * gain_b;
        return wrap(a, std::move(samples));
    }

    Buffer remove_dc(const SampleBuffer &b) {
        std::vector<f32> samples = data(b);
        const usize c = ch(b), frames = static_cast<usize>(b.frames());
        for (const usize k : std::views::iota(usize{0}, c)) {
            const auto channel = channel_of(std::span<f32>{samples}, c, k);
            const f64 sum = Foundation::iter(channel).fold(0.0, [](f64 total, f32 sample) { return total + sample; });
            const f32 mean = frames > 0 ? static_cast<f32>(sum / static_cast<f64>(frames)) : 0.0f;
            for (f32 &sample : channel) sample -= mean;
        }
        return wrap(b, std::move(samples));
    }

    Result normalize(const SampleBuffer &b, const NormalizeOptions &options) {
        if (!b.samples || b.samples->empty()) {
            return std::unexpected("audio: nothing to normalise");
        }
        const f32 peak = Kernels::peak(*b.samples);
        if (peak <= 0.0f) {
            return std::make_shared<SampleBuffer>(b); // silence stays silence
        }
        const f32 peak_db = 20.0f * std::log10(peak);
        f32 change_db;
        if (options.target_lufs) {
            const LoudnessResult loudness = measure_loudness(b);
            if (loudness.integrated <= -70.0) {
                return std::make_shared<SampleBuffer>(b); // below the gate: nothing to measure
            }
            change_db = *options.target_lufs - static_cast<f32>(loudness.integrated);
            change_db = std::min(change_db, options.ceiling_db - peak_db);
        } else {
            change_db = options.target_peak_db - peak_db;
        }
        return gain(b, change_db);
    }

    // ---- silence ----------------------------------------------------------------------------------------------------------------

    std::vector<Range> find_silence(const SampleBuffer &b, f32 threshold_db, f32 min_ms) {
        std::vector<Range> ranges;
        const f32 threshold = std::pow(10.0f, threshold_db / 20.0f);
        const usize c = ch(b), frames = static_cast<usize>(b.frames());
        const u64 min_frames = static_cast<u64>(std::max(0.0f, min_ms) * 0.001f * static_cast<f32>(b.sample_rate));
        const auto &d = data(b);
        u64 run_start = 0;
        bool in_run = false;
        const auto is_quiet = [threshold](std::span<const f32> frame) {
            return std::ranges::all_of(frame, [threshold](f32 sample) { return std::fabs(sample) < threshold; });
        };
        const auto frames_view = frames_of(std::span<const f32>{d}, c);
        for (usize f = 0; f <= frames; ++f) {
            const bool quiet = f < frames && is_quiet(frames_view[f]);
            if (quiet && !in_run) {
                in_run = true;
                run_start = f;
            } else if (!quiet && in_run) {
                in_run = false;
                if (f - run_start >= min_frames) ranges.push_back(Range{run_start, f});
            }
        }
        return ranges;
    }

    Buffer trim_silence(const SampleBuffer &b, f32 threshold_db, f32 keep_ms) {
        const f32 threshold = std::pow(10.0f, threshold_db / 20.0f);
        const usize c = ch(b), frames = static_cast<usize>(b.frames());
        const auto &d = data(b);
        usize first = 0, last = frames;
        const auto frames_view = frames_of(std::span<const f32>{d}, c);
        const auto loud = [&](usize f) {
            return std::ranges::any_of(frames_view[f], [threshold](f32 sample) { return std::fabs(sample) >= threshold; });
        };
        while (first < frames && !loud(first)) ++first;
        if (first == frames) return silence(b, 0);
        while (last > first && !loud(last - 1)) --last;
        const u64 keep = static_cast<u64>(std::max(0.0f, keep_ms) * 0.001f * static_cast<f32>(b.sample_rate));
        return slice(b, first > keep ? first - keep : 0, std::min<u64>(frames, last + keep));
    }

    // ---- channels, rate ---------------------------------------------------------------------------------------------------------------

    Buffer convert_channels(const SampleBuffer &b, u32 channels) {
        if (channels == 0 || channels == b.channels) {
            return std::make_shared<SampleBuffer>(b);
        }
        const ChannelLayoutInfo target_info = ChannelLayoutInfo::guess(channels);
        SpeakerLayout target;
        if (target_info.kind == ChannelKind::Speakers) {
            target = target_info.speakers;
        } else {
            target.speakers.assign(channels, Speaker{ChannelRole::Mono, 0.0f, 0.0f});
        }
        const ChannelLayoutInfo from = b.layout.channels == b.channels ? b.layout : ChannelLayoutInfo::guess(b.channels);
        const ChannelMatrix matrix = make_channel_matrix(from, target);
        const usize frames = static_cast<usize>(b.frames());
        std::vector<f32> samples(frames * channels, 0.0f);
        const auto &d = data(b);
        for_each_frame(std::views::zip(frames_of(std::span<f32>{samples}, channels), frames_of(std::span<const f32>{d}, b.channels)), [&matrix](const auto &pair) {
            const auto [out, in] = pair;
            for (const MatrixTap &t : matrix.taps) {
                out[t.destination] += in[t.source] * t.gain;
            }
        });
        auto out = std::make_shared<SampleBuffer>();
        out->channels = channels;
        out->sample_rate = b.sample_rate;
        out->layout = target_info;
        out->markers = b.markers;
        out->loop = b.loop;
        out->samples = std::make_shared<const std::vector<f32>>(std::move(samples));
        return out;
    }

    Buffer extract_channels(const SampleBuffer &b, std::span<const u32> channels) {
        const usize frames = static_cast<usize>(b.frames());
        const auto &d = data(b);
        std::vector<f32> samples(frames * channels.size());
        for_each_frame(std::views::zip(frames_of(std::span<f32>{samples}, channels.size()), frames_of(std::span<const f32>{d}, b.channels)), [channels](const auto &pair) {
            const auto [out, in] = pair;
            for (const auto [slot, source] : std::views::zip(out, channels)) {
                slot = source < in.size() ? in[source] : 0.0f;
            }
        });
        auto out = std::make_shared<SampleBuffer>();
        out->channels = static_cast<u32>(channels.size());
        out->sample_rate = b.sample_rate;
        out->markers = b.markers;
        out->loop = b.loop;
        out->samples = std::make_shared<const std::vector<f32>>(std::move(samples));
        return out;
    }

    Result combine_channels(std::span<const SampleBuffer *const> tracks) {
        if (tracks.empty()) {
            return std::unexpected("audio: nothing to combine");
        }
        u32 total_channels = 0;
        u64 frames = ~0ull;
        const u32 rate = tracks.front()->sample_rate;
        for (const SampleBuffer *t : tracks) {
            if (t == nullptr || t->sample_rate != rate) {
                return std::unexpected("audio: tracks to combine must share a sample rate");
            }
            total_channels += t->channels;
            frames = std::min(frames, t->frames());
        }
        if (total_channels > max_source_channels) {
            return std::unexpected("audio: too many channels");
        }
        std::vector<f32> samples(static_cast<usize>(frames) * total_channels);
        u32 offset = 0;
        for (const SampleBuffer *t : tracks) {
            const auto &d = data(*t);
            for (const auto [out, in] : std::views::zip(frames_of(std::span<f32>{samples}, total_channels), frames_of(std::span<const f32>{d}, t->channels))) {
                std::ranges::copy(in, out.begin() + offset);
            }
            offset += t->channels;
        }
        auto out = std::make_shared<SampleBuffer>();
        out->channels = total_channels;
        out->sample_rate = rate;
        out->samples = std::make_shared<const std::vector<f32>>(std::move(samples));
        return out;
    }

    Buffer mid_side_encode(const SampleBuffer &b) {
        if (b.channels != 2) return std::make_shared<SampleBuffer>(b);
        std::vector<f32> samples = data(b);
        for (const auto frame : frames_of(std::span<f32>{samples}, 2)) {
            const f32 l = frame[0], r = frame[1];
            frame[0] = 0.5f * (l + r);
            frame[1] = 0.5f * (l - r);
        }
        return wrap(b, std::move(samples));
    }

    Buffer mid_side_decode(const SampleBuffer &b) {
        if (b.channels != 2) return std::make_shared<SampleBuffer>(b);
        std::vector<f32> samples = data(b);
        for (const auto frame : frames_of(std::span<f32>{samples}, 2)) {
            const f32 m = frame[0], s = frame[1];
            frame[0] = m + s;
            frame[1] = m - s;
        }
        return wrap(b, std::move(samples));
    }

    Buffer pan(const SampleBuffer &b, f32 balance) {
        if (b.channels != 2) return std::make_shared<SampleBuffer>(b);
        std::vector<f32> samples = data(b);
        const f32 left = std::min(1.0f, 1.0f - balance), right = std::min(1.0f, 1.0f + balance);
        for (const auto frame : frames_of(std::span<f32>{samples}, 2)) {
            frame[0] *= left;
            frame[1] *= right;
        }
        return wrap(b, std::move(samples));
    }

    Buffer convert_rate(const SampleBuffer &b, u32 sample_rate) {
        if (sample_rate == 0 || sample_rate == b.sample_rate) {
            return std::make_shared<SampleBuffer>(b);
        }
        return resample(b, sample_rate);
    }

    // ---- time and pitch ----------------------------------------------------------------------------------------------------------------

    Buffer change_speed(const SampleBuffer &b, f64 factor) {
        factor = std::clamp(factor, 0.01, 100.0);
        if (std::fabs(factor - 1.0) < 1e-9) {
            return std::make_shared<SampleBuffer>(b);
        }
        // Playing `factor` times faster is the same samples read as if the rate were factor times higher.
        constexpr u32 scale = 64;
        const u32 in_rate = static_cast<u32>(std::max(1.0, std::round(static_cast<f64>(b.sample_rate) * factor * scale)));
        std::vector<f32> samples = resample(data(b), b.channels, in_rate, b.sample_rate * scale, ResampleQuality::High);
        auto out = std::make_shared<SampleBuffer>();
        out->channels = b.channels;
        out->sample_rate = b.sample_rate;
        out->layout = b.layout;
        out->samples = std::make_shared<const std::vector<f32>>(std::move(samples));
        for (const AudioMarker &m : b.markers) out->markers.push_back(AudioMarker{m.name, static_cast<u64>(std::llround(static_cast<f64>(m.frame) / factor))});
        if (b.loop) out->loop = LoopRegion{static_cast<u64>(std::llround(static_cast<f64>(b.loop->start) / factor)), static_cast<u64>(std::llround(static_cast<f64>(b.loop->end) / factor))};
        return out;
    }

    Buffer time_stretch(const SampleBuffer &b, f64 ratio) {
        ratio = std::clamp(ratio, 0.1, 10.0);
        if (std::fabs(ratio - 1.0) < 1e-6 || b.frames() < 64) {
            return std::make_shared<SampleBuffer>(b);
        }
        const usize c = ch(b), n = static_cast<usize>(b.frames());
        const auto &in = data(b);
        // Segment length ~32 ms, overlap-add at half-length hops with a Hann window (the windows sum to one).
        usize frame = 64;
        while (frame < static_cast<usize>(b.sample_rate * 0.032)) frame <<= 1;
        const usize hop_synthesis = frame / 2;
        const f64 hop_analysis = static_cast<f64>(hop_synthesis) / ratio;
        const usize out_frames = static_cast<usize>(std::llround(static_cast<f64>(n) * ratio));
        const isize search = static_cast<isize>(frame / 4);

        std::vector<f32> mono(n);
        for (usize f = 0; f < n; ++f) {
            f32 sum = 0.0f;
            for (usize k = 0; k < c; ++k) sum += in[f * c + k];
            mono[f] = sum / static_cast<f32>(c);
        }
        // A 4x decimated copy makes the broad search cheap; the best candidate is then refined at full rate.
        constexpr usize decimation = 4;
        std::vector<f32> coarse(n / decimation + 1, 0.0f);
        for (usize j = 0; j < coarse.size(); ++j) {
            f32 sum = 0.0f;
            usize count = 0;
            for (usize k = 0; k < decimation && j * decimation + k < n; ++k, ++count) sum += mono[j * decimation + k];
            coarse[j] = count ? sum / static_cast<f32>(count) : 0.0f;
        }
        std::vector<f32> window(frame);
        for (usize i = 0; i < frame; ++i) window[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / static_cast<f32>(frame));

        std::vector<f32> out((out_frames + frame) * c, 0.0f);
        const auto add_segment = [&](usize input_start, usize output_start, bool unwindowed_head) {
            for (usize i = 0; i < frame; ++i) {
                const usize src = input_start + i;
                if (src >= n) break;
                const f32 w = (unwindowed_head && i < hop_synthesis) ? 1.0f : window[i];
                for (usize k = 0; k < c; ++k) out[(output_start + i) * c + k] += in[src * c + k] * w;
            }
        };
        const auto similarity = [&](const std::vector<f32> &signal, usize reference_start, usize candidate_start, usize length) {
            f64 dot = 0.0, energy = 1e-9;
            for (usize i = 0; i < length; ++i) {
                const usize r = reference_start + i, s = candidate_start + i;
                if (r >= signal.size() || s >= signal.size()) break;
                dot += static_cast<f64>(signal[r]) * signal[s];
                energy += static_cast<f64>(signal[s]) * signal[s];
            }
            return dot / std::sqrt(energy);
        };

        add_segment(0, 0, true);
        usize previous = 0;
        for (usize k = 1;; ++k) {
            const usize output_start = k * hop_synthesis;
            if (output_start >= out_frames) break;
            const isize nominal = static_cast<isize>(std::llround(static_cast<f64>(k) * hop_analysis));
            if (nominal >= static_cast<isize>(n)) break;
            const usize reference = previous + hop_synthesis; // where the last segment would have gone on to
            // Coarse search.
            isize best = std::clamp<isize>(nominal, 0, static_cast<isize>(n) - 1);
            f64 best_score = -1e30;
            const isize low = std::max<isize>(0, nominal - search), high = std::min<isize>(static_cast<isize>(n) - 1, nominal + search);
            for (isize s = (low / static_cast<isize>(decimation)) * static_cast<isize>(decimation); s <= high; s += static_cast<isize>(decimation)) {
                const f64 score = similarity(coarse, reference / decimation, static_cast<usize>(std::max<isize>(s, 0)) / decimation, hop_synthesis / decimation);
                if (score > best_score) {
                    best_score = score;
                    best = std::max<isize>(s, 0);
                }
            }
            // Refine at full rate around the coarse winner.
            f64 refined_score = -1e30;
            isize refined = best;
            for (isize s = std::max<isize>(0, best - static_cast<isize>(decimation)); s <= std::min<isize>(static_cast<isize>(n) - 1, best + static_cast<isize>(decimation)); ++s) {
                const f64 score = similarity(mono, reference, static_cast<usize>(s), hop_synthesis);
                if (score > refined_score) {
                    refined_score = score;
                    refined = s;
                }
            }
            previous = static_cast<usize>(refined);
            add_segment(previous, output_start, false);
        }
        out.resize(out_frames * c);
        auto result = std::make_shared<SampleBuffer>();
        result->channels = b.channels;
        result->sample_rate = b.sample_rate;
        result->layout = b.layout;
        for (const AudioMarker &m : b.markers) result->markers.push_back(AudioMarker{m.name, static_cast<u64>(std::llround(static_cast<f64>(m.frame) * ratio))});
        if (b.loop) result->loop = LoopRegion{static_cast<u64>(std::llround(static_cast<f64>(b.loop->start) * ratio)), static_cast<u64>(std::llround(static_cast<f64>(b.loop->end) * ratio))};
        result->samples = std::make_shared<const std::vector<f32>>(std::move(out));
        return result;
    }

    Buffer pitch_shift(const SampleBuffer &b, f64 semitones) {
        const f64 factor = std::pow(2.0, semitones / 12.0);
        if (std::fabs(factor - 1.0) < 1e-9) {
            return std::make_shared<SampleBuffer>(b);
        }
        // Stretch to `factor` times as long at the original pitch, then play that `factor` times faster: pitch moves by
        // `factor`, and the length comes back to what it was.
        const Buffer stretched = time_stretch(b, factor);
        Buffer out = change_speed(*stretched, factor);
        out->markers = b.markers;
        out->loop = b.loop;
        return out;
    }

    // ---- bit depth ----------------------------------------------------------------------------------------------------------------------------

    Buffer quantize(const SampleBuffer &b, u32 bits, bool dither) {
        bits = std::clamp(bits, 1u, 24u);
        std::vector<f32> samples = data(b);
        const f32 step = std::ldexp(2.0f, -static_cast<int>(bits));
        std::mt19937 random(0x5eed);
        std::uniform_real_distribution<f32> uniform(-0.5f, 0.5f);
        for (f32 &s : samples) {
            const f32 noise = dither ? (uniform(random) + uniform(random)) * step : 0.0f;
            s = std::round((s + noise) / step) * step;
        }
        return wrap(b, std::move(samples));
    }

} // namespace SFT::Audio::edit
