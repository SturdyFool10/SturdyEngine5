#include <Audio/Resample.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace SFT::Audio {

    namespace {

        struct QualityParams {
            u32 half_taps;
            f64 beta;
        };

        QualityParams params_for(ResampleQuality q) {
            switch (q) {
                case ResampleQuality::Fast: return {8, 6.0};
                case ResampleQuality::Medium: return {16, 7.8};
                case ResampleQuality::High: return {32, 9.6};
                case ResampleQuality::Best: return {64, 11.5};
            }
            return {32, 9.6};
        }

        // Modified Bessel function of the first kind, order 0 (power series; converges fast for the betas used).
        f64 bessel_i0(f64 x) {
            f64 sum = 1.0, term = 1.0;
            const f64 q = x * x / 4.0;
            for (int k = 1; k < 64; ++k) {
                term *= q / (static_cast<f64>(k) * static_cast<f64>(k));
                sum += term;
                if (term < 1e-14 * sum) {
                    break;
                }
            }
            return sum;
        }

        constexpr u32 kTableResolution = 256;

    } // namespace

    Resampler::Resampler(u32 channels, u32 input_rate, u32 output_rate, ResampleQuality quality)
        : channels_(std::max(channels, 1u)), step_(static_cast<f64>(input_rate) / static_cast<f64>(std::max(output_rate, 1u))),
          table_resolution_(kTableResolution) {
        const QualityParams qp = params_for(quality);
        // Downsampling lowers the cutoff to the output Nyquist, which widens the kernel by the same factor.
        const f64 cutoff = std::min(1.0, 1.0 / step_);
        half_width_ = static_cast<u32>(std::ceil(static_cast<f64>(qp.half_taps) / cutoff));
        const usize entries = static_cast<usize>(2) * half_width_ * table_resolution_ + 1;
        table_.resize(entries);
        const f64 i0_beta = bessel_i0(qp.beta);
        for (usize i = 0; i < entries; ++i) {
            const f64 u = (static_cast<f64>(i) / table_resolution_) - static_cast<f64>(half_width_); // input frames from the centre
            const f64 x = u * cutoff;
            const f64 sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const f64 r = u / static_cast<f64>(half_width_);
            const f64 window = std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(qp.beta * std::sqrt(1.0 - r * r)) / i0_beta;
            table_[i] = static_cast<f32>(cutoff * sinc * window);
        }
        // Small rational ratios get an exact polyphase table.
        u32 a = input_rate, b = std::max(output_rate, 1u);
        while (b != 0) {
            const u32 t = a % b;
            a = b;
            b = t;
        }
        num_ = input_rate / std::max(a, 1u);
        den_ = std::max(output_rate, 1u) / std::max(a, 1u);
        if (den_ <= 1024 && num_ != den_) {
            const usize taps = static_cast<usize>(2) * half_width_;
            phase_table_.resize(static_cast<usize>(den_) * taps);
            for (u32 p = 0; p < den_; ++p) {
                const f64 fraction = static_cast<f64>(p) / static_cast<f64>(den_);
                for (usize k = 0; k < taps; ++k) {
                    const f64 u = static_cast<f64>(static_cast<i64>(k) - static_cast<i64>(half_width_) + 1) - fraction;
                    const f64 x = u * cutoff;
                    const f64 sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                    const f64 r = u / static_cast<f64>(half_width_);
                    const f64 window = std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(qp.beta * std::sqrt(1.0 - r * r)) / i0_beta;
                    phase_table_[static_cast<usize>(p) * taps + k] = static_cast<f32>(cutoff * sinc * window);
                }
            }
            exact_ = true;
        }
        reset();
    }

    void Resampler::reset() {
        exact_base_ = half_width_;
        exact_phase_ = 0;
        // Left padding of silence so the first output sees a full kernel.
        history_.assign(static_cast<usize>(half_width_) * channels_, 0.0f);
        position_ = static_cast<f64>(half_width_);
        exact_active_ = exact_;
    }

    void Resampler::trim() {
        // Frames before position_ - half_width_ can never be needed again.
        const f64 keep_from = std::floor(position_) - static_cast<f64>(half_width_);
        if (keep_from < 1.0) {
            return;
        }
        const usize drop = static_cast<usize>(keep_from);
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop * channels_));
        position_ -= static_cast<f64>(drop);
        exact_base_ = exact_base_ >= drop ? exact_base_ - drop : 0;
    }

    usize Resampler::process(const f32 *input, usize input_frames, std::vector<f32> &output) {
        history_.insert(history_.end(), input, input + input_frames * channels_);
        const usize available = history_.size() / channels_;
        usize produced = 0;
        const f64 step = step_ / adjustment_;
        const bool use_exact = exact_ && adjustment_ == 1.0;
        if (use_exact) {
            // Re-enter exact mode at the current position (it may have been left while the ratio was being nudged).
            if (!exact_active_) {
                exact_base_ = static_cast<u64>(std::floor(position_));
                exact_phase_ = static_cast<u32>(std::lround((position_ - std::floor(position_)) * den_)) % den_;
                exact_active_ = true;
            }
            const usize taps = static_cast<usize>(2) * half_width_;
            while (exact_base_ + half_width_ < available) {
                const usize out_index = output.size();
                output.resize(out_index + channels_, 0.0f);
                const f32 *weights = phase_table_.data() + static_cast<usize>(exact_phase_) * taps;
                const f32 *src = history_.data() + (exact_base_ - half_width_ + 1) * channels_;
                for (usize k = 0; k < taps; ++k) {
                    const f32 w = weights[k];
                    for (u32 c = 0; c < channels_; ++c) {
                        output[out_index + c] += src[k * channels_ + c] * w;
                    }
                }
                exact_phase_ += num_;
                exact_base_ += exact_phase_ / den_;
                exact_phase_ %= den_;
                ++produced;
            }
            position_ = static_cast<f64>(exact_base_) + static_cast<f64>(exact_phase_) / static_cast<f64>(den_);
        } else {
            exact_active_ = false;
            // An output at `position_` reads frames floor(position_) - half_width_ + 1 .. floor(position_) + half_width_.
            while (static_cast<usize>(position_) + half_width_ < available) {
                const i64 base = static_cast<i64>(std::floor(position_));
                const f64 fraction = position_ - static_cast<f64>(base);
                const usize out_index = output.size();
                output.resize(out_index + channels_, 0.0f);
                for (i64 k = -static_cast<i64>(half_width_) + 1; k <= static_cast<i64>(half_width_); ++k) {
                    const i64 frame = base + k;
                    // Kernel argument: distance of that input frame from the output position, in input frames.
                    const f64 u = static_cast<f64>(k) - fraction;
                    const f64 t = (u + static_cast<f64>(half_width_)) * table_resolution_;
                    const usize i = static_cast<usize>(t);
                    if (i + 1 >= table_.size()) {
                        continue;
                    }
                    const f32 weight = table_[i] + (table_[i + 1] - table_[i]) * static_cast<f32>(t - static_cast<f64>(i));
                    const f32 *src = history_.data() + static_cast<usize>(frame) * channels_;
                    for (u32 c = 0; c < channels_; ++c) {
                        output[out_index + c] += src[c] * weight;
                    }
                }
                position_ += step;
                ++produced;
            }
        }
        trim();
        return produced;
    }

    usize Resampler::flush(std::vector<f32> &output) {
        const std::vector<f32> silence(static_cast<usize>(half_width_) * channels_, 0.0f);
        return process(silence.data(), half_width_, output);
    }

    std::vector<f32> resample(std::span<const f32> interleaved, u32 channels, u32 input_rate, u32 output_rate, ResampleQuality quality) {
        if (input_rate == output_rate || channels == 0 || input_rate == 0 || output_rate == 0) {
            return std::vector<f32>(interleaved.begin(), interleaved.end());
        }
        Resampler resampler(channels, input_rate, output_rate, quality);
        std::vector<f32> out;
        const usize frames = interleaved.size() / channels;
        out.reserve(static_cast<usize>(static_cast<f64>(frames) * output_rate / input_rate + 64) * channels);
        resampler.process(interleaved.data(), frames, out);
        resampler.flush(out);
        // The filter's own delay shifts the output; drop that many leading frames and clamp the length to the exact count.
        const usize delay = static_cast<usize>(std::lround(resampler.latency_frames() * static_cast<f64>(output_rate) / input_rate));
        const usize exact = static_cast<usize>(std::llround(static_cast<f64>(frames) * output_rate / input_rate));
        // (the history was pre-padded by half_width input frames, so output frame 0 already corresponds to input frame 0)
        (void)delay;
        out.resize(std::min(out.size(), exact * channels));
        return out;
    }

    std::shared_ptr<SampleBuffer> resample(const SampleBuffer &buffer, u32 output_rate, ResampleQuality quality) {
        auto out = std::make_shared<SampleBuffer>();
        out->channels = buffer.channels;
        out->sample_rate = output_rate;
        if (buffer.samples) {
            out->samples = std::make_shared<const std::vector<f32>>(resample(*buffer.samples, buffer.channels, buffer.sample_rate, output_rate, quality));
        }
        const f64 scale = buffer.sample_rate > 0 ? static_cast<f64>(output_rate) / buffer.sample_rate : 1.0;
        for (const AudioMarker &m : buffer.markers) {
            out->markers.push_back(AudioMarker{m.name, static_cast<u64>(std::llround(static_cast<f64>(m.frame) * scale))});
        }
        if (buffer.loop) {
            out->loop = LoopRegion{static_cast<u64>(std::llround(static_cast<f64>(buffer.loop->start) * scale)),
                                   static_cast<u64>(std::llround(static_cast<f64>(buffer.loop->end) * scale))};
        }
        return out;
    }

} // namespace SFT::Audio
