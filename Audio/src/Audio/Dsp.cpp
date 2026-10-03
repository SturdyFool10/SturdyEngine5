#include <Audio/Dsp.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <complex>

namespace SFT::Audio {

    namespace {
        constexpr f32 kPi = glm::pi<f32>();
        constexpr f32 kTwoPi = glm::two_pi<f32>();

        // One-pole coefficient reaching ~63% of the target in `seconds`.
        f32 time_constant_coefficient(f32 sample_rate, f32 seconds) noexcept {
            return seconds <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (seconds * sample_rate));
        }
    } // namespace

    // ---- Biquad ----------------------------------------------------------------------------------------------

    BiquadCoefficients design_biquad(FilterType type, f32 sample_rate, f32 frequency, f32 q, f32 gain_db) noexcept {
        frequency = std::clamp(frequency, 1.0f, sample_rate * 0.49f);
        q = std::max(q, 0.01f);
        const f32 w0 = kTwoPi * frequency / sample_rate;
        const f32 cos_w = std::cos(w0), sin_w = std::sin(w0);
        const f32 alpha = sin_w / (2.0f * q);
        const f32 A = std::pow(10.0f, gain_db / 40.0f);
        f32 b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type) {
            case FilterType::LowPass:
                b0 = (1 - cos_w) / 2; b1 = 1 - cos_w; b2 = (1 - cos_w) / 2;
                a0 = 1 + alpha; a1 = -2 * cos_w; a2 = 1 - alpha;
                break;
            case FilterType::HighPass:
                b0 = (1 + cos_w) / 2; b1 = -(1 + cos_w); b2 = (1 + cos_w) / 2;
                a0 = 1 + alpha; a1 = -2 * cos_w; a2 = 1 - alpha;
                break;
            case FilterType::BandPass:
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0 = 1 + alpha; a1 = -2 * cos_w; a2 = 1 - alpha;
                break;
            case FilterType::Notch:
                b0 = 1; b1 = -2 * cos_w; b2 = 1;
                a0 = 1 + alpha; a1 = -2 * cos_w; a2 = 1 - alpha;
                break;
            case FilterType::AllPass:
                b0 = 1 - alpha; b1 = -2 * cos_w; b2 = 1 + alpha;
                a0 = 1 + alpha; a1 = -2 * cos_w; a2 = 1 - alpha;
                break;
            case FilterType::Peak:
                b0 = 1 + alpha * A; b1 = -2 * cos_w; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cos_w; a2 = 1 - alpha / A;
                break;
            case FilterType::LowShelf: {
                const f32 beta = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cos_w + beta);
                b1 = 2 * A * ((A - 1) - (A + 1) * cos_w);
                b2 = A * ((A + 1) - (A - 1) * cos_w - beta);
                a0 = (A + 1) + (A - 1) * cos_w + beta;
                a1 = -2 * ((A - 1) + (A + 1) * cos_w);
                a2 = (A + 1) + (A - 1) * cos_w - beta;
                break;
            }
            case FilterType::HighShelf: {
                const f32 beta = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cos_w + beta);
                b1 = -2 * A * ((A - 1) + (A + 1) * cos_w);
                b2 = A * ((A + 1) + (A - 1) * cos_w - beta);
                a0 = (A + 1) - (A - 1) * cos_w + beta;
                a1 = 2 * ((A - 1) - (A + 1) * cos_w);
                a2 = (A + 1) - (A - 1) * cos_w - beta;
                break;
            }
        }
        return BiquadCoefficients{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    void Biquad::set(FilterType type, f32 sample_rate, f32 frequency, f32 q, f32 gain_db) noexcept {
        set(design_biquad(type, sample_rate, frequency, q, gain_db));
    }

    void Biquad::process(std::span<f32> samples) noexcept {
        for (f32 &s : samples) {
            s = process(s);
        }
    }

    f32 Biquad::magnitude_at(f32 frequency, f32 sample_rate) const noexcept {
        const f32 w = kTwoPi * frequency / sample_rate;
        const std::complex<f32> z1 = std::polar(1.0f, -w), z2 = std::polar(1.0f, -2.0f * w);
        const std::complex<f32> numerator = b0_ + b1_ * z1 + b2_ * z2;
        const std::complex<f32> denominator = 1.0f + a1_ * z1 + a2_ * z2;
        return std::abs(numerator / denominator);
    }

    // ---- State-variable filter ---------------------------------------------------------------------------------

    void StateVariableFilter::set(f32 sample_rate, f32 cutoff, f32 q) noexcept {
        cutoff = std::clamp(cutoff, 1.0f, sample_rate * 0.49f);
        const f32 g = std::tan(kPi * cutoff / sample_rate);
        k_ = 1.0f / std::max(q, 0.01f);
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    StateVariableFilter::Output StateVariableFilter::process(f32 x) noexcept {
        const f32 v3 = x - ic2_;
        const f32 v1 = a1_ * ic1_ + a2_ * v3;
        const f32 v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, x - k_ * v1 - v2};
    }

    // ---- Oscillator --------------------------------------------------------------------------------------------

    namespace {
        // Polynomial band-limited step correction around the discontinuity at phase 0 (width `dt`).
        f32 poly_blep(f32 t, f32 dt) noexcept {
            if (t < dt) {
                t /= dt;
                return t + t - t * t - 1.0f;
            }
            if (t > 1.0f - dt) {
                t = (t - 1.0f) / dt;
                return t * t + t + t + 1.0f;
            }
            return 0.0f;
        }
    } // namespace

    void Oscillator::set(Waveform waveform, f32 sample_rate, f32 frequency, f32 pulse_width) noexcept {
        waveform_ = waveform;
        sample_rate_ = sample_rate;
        pulse_width_ = std::clamp(pulse_width, 0.01f, 0.99f);
        set_frequency(frequency);
    }

    void Oscillator::set_frequency(f32 frequency) noexcept {
        increment_ = std::clamp(frequency / sample_rate_, 0.0f, 0.5f);
    }

    f32 Oscillator::next() noexcept {
        f32 out = 0.0f;
        switch (waveform_) {
            case Waveform::Sine: out = std::sin(kTwoPi * phase_); break;
            case Waveform::Saw: out = 2.0f * phase_ - 1.0f - poly_blep(phase_, increment_); break;
            case Waveform::Square: {
                out = phase_ < pulse_width_ ? 1.0f : -1.0f;
                out += poly_blep(phase_, increment_);
                out -= poly_blep(std::fmod(phase_ + 1.0f - pulse_width_, 1.0f), increment_);
                break;
            }
            case Waveform::Triangle: out = 4.0f * std::fabs(phase_ - 0.5f) - 1.0f; break;
        }
        phase_ += increment_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }
        return out;
    }

    // ---- Noise -------------------------------------------------------------------------------------------------

    f32 Noise::white() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<f32>(static_cast<i32>(state_)) * (1.0f / 2147483648.0f);
    }

    f32 Noise::pink() noexcept {
        const f32 w = white();
        b_[0] = 0.99886f * b_[0] + w * 0.0555179f;
        b_[1] = 0.99332f * b_[1] + w * 0.0750759f;
        b_[2] = 0.96900f * b_[2] + w * 0.1538520f;
        b_[3] = 0.86650f * b_[3] + w * 0.3104856f;
        b_[4] = 0.55000f * b_[4] + w * 0.5329522f;
        b_[5] = -0.7616f * b_[5] - w * 0.0168980f;
        const f32 out = b_[0] + b_[1] + b_[2] + b_[3] + b_[4] + b_[5] + b_[6] + w * 0.5362f;
        b_[6] = w * 0.115926f;
        return out * 0.11f;
    }

    // ---- Envelope ----------------------------------------------------------------------------------------------

    void Envelope::set(f32 sample_rate, f32 attack_seconds, f32 decay_seconds, f32 sustain_level, f32 release_seconds) noexcept {
        attack_coefficient_ = time_constant_coefficient(sample_rate, attack_seconds * 0.3f);
        decay_coefficient_ = time_constant_coefficient(sample_rate, decay_seconds * 0.3f);
        release_coefficient_ = time_constant_coefficient(sample_rate, release_seconds * 0.3f);
        sustain_ = std::clamp(sustain_level, 0.0f, 1.0f);
    }

    f32 Envelope::next() noexcept {
        switch (stage_) {
            case Stage::Idle: break;
            case Stage::Attack:
                // Aim past 1 so the exponential reaches it in finite time.
                level_ += (1.2f - level_) * attack_coefficient_;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                level_ += (sustain_ - level_) * decay_coefficient_;
                if (std::fabs(level_ - sustain_) < 1e-4f) {
                    level_ = sustain_;
                    stage_ = Stage::Sustain;
                }
                break;
            case Stage::Sustain: level_ = sustain_; break;
            case Stage::Release:
                level_ += (0.0f - level_) * release_coefficient_;
                if (level_ < 1e-4f) {
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                }
                break;
        }
        return level_;
    }

    // ---- Delay line --------------------------------------------------------------------------------------------

    void DelayLine::resize(u32 max_samples) {
        u32 size = 4;
        while (size < max_samples + 2) {
            size <<= 1;
        }
        buffer_.assign(size, 0.0f);
        mask_ = size - 1;
        write_index_ = 0;
    }

    void DelayLine::clear() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_index_ = 0;
    }

    f32 DelayLine::read(f32 delay) const noexcept {
        if (buffer_.empty()) {
            return 0.0f;
        }
        delay = std::clamp(delay, 1.0f, static_cast<f32>(buffer_.size() - 2));
        const u32 whole = static_cast<u32>(delay);
        const f32 fraction = delay - static_cast<f32>(whole);
        const f32 a = buffer_[(write_index_ - whole) & mask_];
        const f32 b = buffer_[(write_index_ - whole - 1) & mask_];
        return a + (b - a) * fraction;
    }

    // ---- FDN reverb --------------------------------------------------------------------------------------------

    void FdnReverb::set(f32 sample_rate, f32 rt60, f32 size, f32 damping) noexcept {
        static constexpr std::array<f32, lines> base_ms{29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.3f, 71.9f};
        sample_rate_ = sample_rate;
        rt60_ = std::max(rt60, 0.05f);
        damping_ = std::clamp(damping, 0.0f, 0.98f);
        size = std::max(size, 0.1f);
        for (u32 i = 0; i < lines; ++i) {
            lengths_[i] = std::max(2.0f, base_ms[i] * 0.001f * sample_rate * size);
            if (delays_[i].capacity() < static_cast<u32>(lengths_[i]) + 4) {
                delays_[i].resize(static_cast<u32>(lengths_[i]) + 4);
            }
            // Gain that loses 60 dB over rt60 seconds: 10^(-3 * delay_seconds / rt60).
            gains_[i] = std::pow(10.0f, -3.0f * (lengths_[i] / sample_rate) / rt60_);
        }
    }

    void FdnReverb::clear() noexcept {
        for (auto &d : delays_) {
            d.clear();
        }
        damp_state_.fill(0.0f);
    }

    void FdnReverb::process(const f32 *mono_in, f32 *out_left, f32 *out_right, u32 frames) noexcept {
        constexpr f32 scale = 0.35355339f; // 1 / sqrt(8): keeps the mixing matrix energy preserving
        for (u32 n = 0; n < frames; ++n) {
            std::array<f32, lines> y;
            for (u32 i = 0; i < lines; ++i) {
                y[i] = delays_[i].read(lengths_[i]);
            }
            f32 left = 0.0f, right = 0.0f;
            for (u32 i = 0; i < lines; ++i) {
                (i & 1u ? right : left) += y[i];
            }
            out_left[n] = left * 0.5f;
            out_right[n] = right * 0.5f;

            // Hadamard butterflies (8-point, in place).
            std::array<f32, lines> m = y;
            for (u32 span = 1; span < lines; span <<= 1) {
                for (u32 base = 0; base < lines; base += span << 1) {
                    for (u32 k = base; k < base + span; ++k) {
                        const f32 a = m[k], b = m[k + span];
                        m[k] = a + b;
                        m[k + span] = a - b;
                    }
                }
            }
            for (u32 i = 0; i < lines; ++i) {
                f32 feedback = m[i] * scale * gains_[i];
                damp_state_[i] = feedback * (1.0f - damping_) + damp_state_[i] * damping_;
                delays_[i].write(damp_state_[i] + mono_in[n] * 0.25f);
            }
        }
    }

    // ---- Karplus-Strong ----------------------------------------------------------------------------------------

    void KarplusStrong::pluck(f32 sample_rate, f32 frequency, f32 decay, f32 brightness, u32 seed) noexcept {
        length_ = std::max<u32>(2, static_cast<u32>(sample_rate / std::max(frequency, 20.0f)));
        line_.assign(length_, 0.0f);
        Noise noise(seed);
        f32 low = 0.0f;
        for (u32 i = 0; i < length_; ++i) {
            const f32 w = noise.white();
            low += (w - low) * (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * 0.9f; // darker bursts for low brightness
            line_[i] = std::clamp(brightness, 0.0f, 1.0f) * w + (1.0f - std::clamp(brightness, 0.0f, 1.0f)) * low;
        }
        position_ = 0;
        decay_ = std::clamp(decay, 0.0f, 0.99999f);
        previous_ = 0.0f;
    }

    f32 KarplusStrong::next() noexcept {
        if (length_ == 0) {
            return 0.0f;
        }
        const f32 out = line_[position_];
        line_[position_] = decay_ * 0.5f * (out + previous_);
        previous_ = out;
        position_ = (position_ + 1) % length_;
        return out;
    }

    // ---- Modal bank --------------------------------------------------------------------------------------------

    void ModalBank::set(f32 sample_rate, std::span<const Mode> modes) {
        modes_.clear();
        modes_.reserve(modes.size());
        for (const Mode &mode : modes) {
            if (mode.frequency <= 0.0f || mode.frequency >= sample_rate * 0.5f) {
                continue; // above Nyquist cannot ring
            }
            State s;
            const f32 w = kTwoPi * mode.frequency / sample_rate;
            // Amplitude falls by 10^-3 after decay_seconds: r^(decay * sr) = e^-6.9078.
            const f32 r = std::exp(-6.9077553f / (std::max(mode.decay_seconds, 1e-3f) * sample_rate));
            s.a1 = 2.0f * r * std::cos(w);
            s.a2 = -r * r;
            s.input_gain = mode.gain * std::sin(w); // unit-amplitude impulse response regardless of frequency
            modes_.push_back(s);
        }
        pending_ = 0.0f;
    }

    f32 ModalBank::next() noexcept {
        const f32 x = pending_;
        pending_ = 0.0f;
        f32 sum = 0.0f;
        for (State &m : modes_) {
            const f32 y = m.a1 * m.y1 + m.a2 * m.y2 + x * m.input_gain;
            m.y2 = m.y1;
            m.y1 = y;
            sum += y;
        }
        return sum;
    }

    bool ModalBank::silent() const noexcept {
        if (pending_ != 0.0f) {
            return false;
        }
        for (const State &m : modes_) {
            if (std::fabs(m.y1) > 1e-6f || std::fabs(m.y2) > 1e-6f) {
                return false;
            }
        }
        return true;
    }

    // ---- Dynamics ----------------------------------------------------------------------------------------------

    void Limiter::set(f32 sample_rate, f32 ceiling, f32 release_seconds) noexcept {
        ceiling_ = std::max(ceiling, 0.01f);
        release_coefficient_ = time_constant_coefficient(sample_rate, release_seconds);
    }

    void Limiter::process(AudioBuffer &buffer) noexcept {
        const u32 channels = buffer.channels(), frames = buffer.frames();
        for (u32 n = 0; n < frames; ++n) {
            f32 peak = 0.0f;
            for (u32 c = 0; c < channels; ++c) {
                peak = std::max(peak, std::fabs(buffer.data(c)[n]));
            }
            const f32 target = peak > ceiling_ ? ceiling_ / peak : 1.0f;
            // Attack is instant; the release never climbs past what this sample needs.
            gain_ = std::min(target, target < gain_ ? target : gain_ + (1.0f - gain_) * release_coefficient_);
            for (u32 c = 0; c < channels; ++c) {
                buffer.data(c)[n] *= gain_;
            }
        }
    }

    void Compressor::set(f32 sample_rate, f32 threshold_db, f32 ratio, f32 attack_seconds, f32 release_seconds, f32 makeup_db) noexcept {
        threshold_db_ = threshold_db;
        ratio_ = std::max(ratio, 1.0f);
        attack_coefficient_ = time_constant_coefficient(sample_rate, attack_seconds);
        release_coefficient_ = time_constant_coefficient(sample_rate, release_seconds);
        makeup_ = db_to_linear(makeup_db);
    }

    void Compressor::process(AudioBuffer &buffer) noexcept {
        const u32 channels = buffer.channels(), frames = buffer.frames();
        for (u32 n = 0; n < frames; ++n) {
            f32 peak = 0.0f;
            for (u32 c = 0; c < channels; ++c) {
                peak = std::max(peak, std::fabs(buffer.data(c)[n]));
            }
            const f32 level_db = linear_to_db(peak);
            const f32 coefficient = level_db > envelope_db_ ? attack_coefficient_ : release_coefficient_;
            envelope_db_ += (level_db - envelope_db_) * coefficient;
            const f32 over = envelope_db_ - threshold_db_;
            const f32 reduction_db = over > 0.0f ? over * (1.0f - 1.0f / ratio_) : 0.0f;
            const f32 gain = db_to_linear(-reduction_db) * makeup_;
            for (u32 c = 0; c < channels; ++c) {
                buffer.data(c)[n] *= gain;
            }
        }
    }

} // namespace SFT::Audio
