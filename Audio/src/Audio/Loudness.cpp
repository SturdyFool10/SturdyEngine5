#include <Audio/Loudness.hpp>

#include <Audio/Resample.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace SFT::Audio {

    namespace {

        constexpr f64 kOffset = -0.691;

        f64 to_lufs(f64 mean_square) { return mean_square > 0.0 ? kOffset + 10.0 * std::log10(mean_square) : -200.0; }

        f64 weight_for(ChannelRole role) {
            switch (role) {
                case ChannelRole::Lfe: case ChannelRole::Lfe2: return 0.0;
                case ChannelRole::SideLeft: case ChannelRole::SideRight: case ChannelRole::RearLeft: case ChannelRole::RearRight:
                case ChannelRole::BackCenter: return 1.41253754462275; // +1.5 dB
                default: return 1.0;
            }
        }

    } // namespace

    LoudnessMeter::LoudnessMeter(u32 channels, u32 sample_rate, const ChannelLayoutInfo &layout)
        : channels_(std::max(channels, 1u)), sample_rate_(std::max(sample_rate, 1u)), step_frames_(std::max(1u, sample_rate / 10)) {
        weights_.assign(channels_, 1.0);
        if (layout.kind == ChannelKind::Speakers && layout.speakers.channel_count() == channels_) {
            for (u32 c = 0; c < channels_; ++c) {
                weights_[c] = weight_for(layout.speakers.speakers[c].role);
            }
        }
        // K-weighting: a high shelf (+4 dB above ~1.7 kHz, the head's acoustic effect) then a ~38 Hz high-pass (RLB), with the
        // BS.1770 analogue prototypes mapped to the actual sample rate by the bilinear transform.
        const f64 fs = static_cast<f64>(sample_rate_);
        Biquad2 shelf, hp;
        {
            const f64 f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            const f64 k = std::tan(std::numbers::pi * f0 / fs);
            const f64 vh = std::pow(10.0, gain / 20.0);
            const f64 vb = std::pow(vh, 0.4996667741545416);
            const f64 a0 = 1.0 + k / q + k * k;
            shelf.b0 = (vh + vb * k / q + k * k) / a0;
            shelf.b1 = 2.0 * (k * k - vh) / a0;
            shelf.b2 = (vh - vb * k / q + k * k) / a0;
            shelf.a1 = 2.0 * (k * k - 1.0) / a0;
            shelf.a2 = (1.0 - k / q + k * k) / a0;
        }
        {
            const f64 f0 = 38.13547087602444, q = 0.5003270373238773;
            const f64 k = std::tan(std::numbers::pi * f0 / fs);
            const f64 a0 = 1.0 + k / q + k * k;
            hp.b0 = 1.0;
            hp.b1 = -2.0;
            hp.b2 = 1.0;
            hp.a1 = 2.0 * (k * k - 1.0) / a0;
            hp.a2 = (1.0 - k / q + k * k) / a0;
        }
        shelf_.assign(channels_, shelf);
        high_pass_.assign(channels_, hp);
    }

    void LoudnessMeter::reset() {
        for (auto &f : shelf_) f.z1 = f.z2 = 0.0;
        for (auto &f : high_pass_) f.z1 = f.z2 = 0.0;
        steps_.clear();
        step_fill_ = 0;
        step_energy_ = 0.0;
        sample_peak_ = 0.0;
    }

    void LoudnessMeter::process(const f32 *interleaved, usize frames) {
        for (usize i = 0; i < frames; ++i) {
            f64 energy = 0.0;
            for (u32 c = 0; c < channels_; ++c) {
                const f64 x = static_cast<f64>(interleaved[i * channels_ + c]);
                sample_peak_ = std::max(sample_peak_, std::fabs(x));
                if (weights_[c] == 0.0) {
                    continue;
                }
                const f64 y = high_pass_[c].step(shelf_[c].step(x));
                energy += weights_[c] * y * y;
            }
            step_energy_ += energy;
            if (++step_fill_ == step_frames_) {
                steps_.push_back(step_energy_ / static_cast<f64>(step_frames_));
                step_energy_ = 0.0;
                step_fill_ = 0;
            }
        }
    }

    namespace {
        // Mean power of the last `count` 100 ms steps.
        f64 window_power(const std::vector<f64> &steps, usize end, usize count) {
            if (end < count) {
                return 0.0;
            }
            f64 sum = 0.0;
            for (usize i = end - count; i < end; ++i) sum += steps[i];
            return sum / static_cast<f64>(count);
        }
    } // namespace

    f64 LoudnessMeter::momentary() const { return to_lufs(window_power(steps_, steps_.size(), 4)); }
    f64 LoudnessMeter::short_term() const { return to_lufs(window_power(steps_, steps_.size(), 30)); }

    LoudnessResult LoudnessMeter::result() const {
        LoudnessResult r;
        r.sample_peak_db = sample_peak_ > 0.0 ? 20.0 * std::log10(sample_peak_) : -200.0;
        // 400 ms blocks every 100 ms (75% overlap).
        std::vector<f64> blocks;
        for (usize end = 4; end <= steps_.size(); ++end) {
            const f64 power = window_power(steps_, end, 4);
            blocks.push_back(power);
            r.momentary_max = std::max(r.momentary_max, to_lufs(power));
        }
        std::vector<f64> short_blocks;
        for (usize end = 30; end <= steps_.size(); ++end) {
            const f64 power = window_power(steps_, end, 30);
            short_blocks.push_back(power);
            r.short_term_max = std::max(r.short_term_max, to_lufs(power));
        }
        // Integrated: absolute gate at -70 LUFS, then a relative gate 10 LU below the gated mean.
        const auto gated_mean = [](const std::vector<f64> &values, f64 threshold_lufs) {
            f64 sum = 0.0;
            usize n = 0;
            for (f64 v : values) {
                if (to_lufs(v) > threshold_lufs) {
                    sum += v;
                    ++n;
                }
            }
            return n > 0 ? sum / static_cast<f64>(n) : 0.0;
        };
        const f64 absolute_mean = gated_mean(blocks, -70.0);
        if (absolute_mean > 0.0) {
            const f64 relative_threshold = to_lufs(absolute_mean) - 10.0;
            const f64 mean = gated_mean(blocks, relative_threshold);
            r.integrated = mean > 0.0 ? to_lufs(mean) : -70.0;
        }
        // Loudness range: the spread between the 10th and 95th percentile of 3 s windows above a -20 LU relative gate.
        const f64 short_absolute = gated_mean(short_blocks, -70.0);
        if (short_absolute > 0.0) {
            const f64 relative = to_lufs(short_absolute) - 20.0;
            std::vector<f64> levels;
            for (f64 v : short_blocks) {
                const f64 l = to_lufs(v);
                if (l > -70.0 && l > relative) levels.push_back(l);
            }
            if (levels.size() >= 2) {
                std::sort(levels.begin(), levels.end());
                const auto at = [&](f64 p) { return levels[std::min(levels.size() - 1, static_cast<usize>(p * static_cast<f64>(levels.size() - 1) + 0.5))]; };
                r.range = std::max(0.0, at(0.95) - at(0.10));
            }
        }
        return r;
    }

    LoudnessResult measure_loudness(const SampleBuffer &buffer) {
        if (!buffer.samples || buffer.channels == 0 || buffer.sample_rate == 0) {
            return {};
        }
        LoudnessMeter meter(buffer.channels, buffer.sample_rate, buffer.layout.channels == buffer.channels ? buffer.layout : ChannelLayoutInfo::guess(buffer.channels));
        meter.process(buffer.samples->data(), static_cast<usize>(buffer.frames()));
        LoudnessResult result = meter.result();
        // True peak: the highest value the reconstructed analogue signal reaches, found by 4x oversampling.
        const std::vector<f32> up = resample(*buffer.samples, buffer.channels, buffer.sample_rate, buffer.sample_rate * 4, ResampleQuality::Medium);
        f32 peak = 0.0f;
        for (f32 s : up) peak = std::max(peak, std::fabs(s));
        result.true_peak_db = std::max(result.sample_peak_db, peak > 0.0f ? 20.0 * std::log10(static_cast<f64>(peak)) : -200.0);
        return result;
    }

} // namespace SFT::Audio
