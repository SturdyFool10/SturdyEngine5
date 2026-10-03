#include <Audio/Effects.hpp>

#include <Audio/Denoise.hpp>
#include <Audio/EffectBase.hpp>
#include <Audio/Fft.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Resample.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace SFT::Audio {

    // ---- AudioEffect ------------------------------------------------------------------------------------------------

    u32 AudioEffect::find_parameter(const ustr &name) const {
        const auto infos = parameters();
        for (u32 i = 0; i < infos.size(); ++i) {
            if (infos[i].name == name) {
                return i;
            }
        }
        return static_cast<u32>(infos.size());
    }

    bool AudioEffect::set_parameter(const ustr &name, f32 value) {
        const u32 index = find_parameter(name);
        if (index >= parameter_count()) {
            return false;
        }
        set_parameter(index, value);
        return true;
    }

    namespace {

        constexpr f32 kPi = std::numbers::pi_v<f32>;

        using detail::ParametricEffect;

        const UString kOffOn[] = {"Off", "On"};

        // ---- shared building blocks -------------------------------------------------------------------------------------

        struct Section {
            f32 b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        };

        Section to_section(const BiquadCoefficients &c) { return Section{c.b0, c.b1, c.b2, c.a1, c.a2}; }

        Section first_order(bool high, f32 sample_rate, f32 frequency) {
            const f32 k = std::tan(kPi * std::clamp(frequency, 1.0f, sample_rate * 0.49f) / sample_rate);
            Section s;
            const f32 norm = 1.0f / (1.0f + k);
            s.a1 = (k - 1.0f) * norm;
            if (high) {
                s.b0 = norm;
                s.b1 = -norm;
            } else {
                s.b0 = k * norm;
                s.b1 = k * norm;
            }
            return s;
        }

        /// Butterworth low/high-pass of `order` (1-8) as a cascade; a second-order filter takes the caller's Q so it can
        /// resonate, higher orders use the Butterworth pole Qs. Returns the sections written to `out` (at most 4 + 1).
        usize design_butterworth(bool high, f32 sample_rate, f32 frequency, u32 order, f32 q, Section *out) {
            usize count = 0;
            order = std::clamp(order, 1u, 8u);
            if (order == 2) {
                out[count++] = to_section(design_biquad(high ? FilterType::HighPass : FilterType::LowPass, sample_rate, frequency, q));
                return count;
            }
            if (order % 2 == 1) {
                out[count++] = first_order(high, sample_rate, frequency);
            }
            for (u32 k = 1; k <= order / 2; ++k) {
                const f32 pole_q = 1.0f / (2.0f * std::cos(kPi * static_cast<f32>(2 * k - 1) / static_cast<f32>(2 * order)));
                out[count++] = to_section(design_biquad(high ? FilterType::HighPass : FilterType::LowPass, sample_rate, frequency, pole_q));
            }
            return count;
        }

        /// A series of biquads with per-channel state. Coefficients can be replaced while running without touching state.
        class Cascade {
          public:
            void allocate(u32 channels, u32 max_sections) {
                channels_ = channels;
                max_sections_ = max_sections;
                sections_.assign(max_sections, Section{});
                state_.assign(static_cast<usize>(channels) * max_sections * 2, 0.0f);
                count_ = 0;
            }
            void set(std::span<const Section> sections) {
                count_ = std::min<usize>(sections.size(), max_sections_);
                std::copy_n(sections.begin(), count_, sections_.begin());
            }
            void reset() { std::fill(state_.begin(), state_.end(), 0.0f); }
            void process(AudioBuffer &buffer) noexcept {
                const u32 channels = std::min(channels_, buffer.channels());
                const u32 frames = buffer.frames();
                for (u32 c = 0; c < channels; ++c) {
                    f32 *x = buffer.data(c);
                    for (usize s = 0; s < count_; ++s) {
                        const Section &k = sections_[s];
                        f32 *z = state_.data() + (static_cast<usize>(c) * max_sections_ + s) * 2;
                        f32 z1 = z[0], z2 = z[1];
                        for (u32 i = 0; i < frames; ++i) {
                            const f32 in = x[i];
                            const f32 out = k.b0 * in + z1;
                            z1 = k.b1 * in - k.a1 * out + z2;
                            z2 = k.b2 * in - k.a2 * out;
                            x[i] = out;
                        }
                        z[0] = z1;
                        z[1] = z2;
                    }
                }
            }

          private:
            u32 channels_ = 0;
            usize max_sections_ = 0;
            usize count_ = 0;
            std::vector<Section> sections_;
            std::vector<f32> state_;
        };

        /// Fixed-length sample delay per channel (a ring that returns what was pushed `length` samples ago).
        class SampleDelay {
          public:
            void allocate(u32 channels, u32 max_length) {
                capacity_ = max_length + 1;
                ring_.assign(static_cast<usize>(channels) * capacity_, 0.0f);
                heads_.assign(channels, 0);
                length_ = 0;
            }
            void set_length(u32 length) {
                length_ = std::min<u32>(length, capacity_ - 1);
            }
            void reset() {
                std::fill(ring_.begin(), ring_.end(), 0.0f);
                std::fill(heads_.begin(), heads_.end(), 0u);
            }
            [[nodiscard]] u32 length() const noexcept { return length_; }
            f32 push_pop(u32 channel, f32 x) noexcept {
                if (length_ == 0) {
                    return x;
                }
                f32 *ring = ring_.data() + static_cast<usize>(channel) * capacity_;
                u32 &head = heads_[channel];
                ring[head] = x;
                const u32 read = head >= length_ ? head - length_ : head + capacity_ - length_;
                head = head + 1 == capacity_ ? 0 : head + 1;
                return ring[read];
            }

          private:
            u32 capacity_ = 1;
            u32 length_ = 0;
            std::vector<f32> ring_;
            std::vector<u32> heads_;
        };

        /// First-order high-pass used to keep rumble out of dynamics detectors.
        struct DetectorFilter {
            std::vector<f32> low;
            f32 coefficient = 0.0f;
            bool enabled = false;
            void allocate(u32 channels) { low.assign(channels, 0.0f); }
            void set(f32 sample_rate, f32 cutoff) {
                enabled = cutoff > 1.0f;
                coefficient = enabled ? 1.0f - std::exp(-2.0f * kPi * cutoff / sample_rate) : 0.0f;
            }
            f32 process(u32 channel, f32 x) noexcept {
                if (!enabled) {
                    return x;
                }
                low[channel] += (x - low[channel]) * coefficient;
                return x - low[channel];
            }
            void reset() { std::fill(low.begin(), low.end(), 0.0f); }
        };

        // ================================================================================================================
        // Filter
        // ================================================================================================================

        const UString kFilterTypes[] = {"Low-pass", "High-pass", "Band-pass", "Notch", "All-pass", "Peak", "Low shelf", "High shelf"};
        const ParameterInfo kFilterParams[] = {
            {"type", "", 0, 7, 0, false, kFilterTypes},
            {"frequency", "Hz", 10, 22000, 1000, true},
            {"q", "", 0.1f, 20, 0.7071f},
            {"gain", "dB", -24, 24, 0},
            {"order", "", 1, 8, 2},
        };

        class FilterEffect final : public ParametricEffect {
          public:
            FilterEffect() : ParametricEffect(kFilterParams) {}
            [[nodiscard]] ustr name() const override { return "Filter"_ustr; }
            void on_prepare() override { cascade_.allocate(channels_, 6); }
            void reset() override { cascade_.reset(); }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    redesign();
                }
                cascade_.process(buffer);
            }

          private:
            void redesign() {
                const auto type = static_cast<FilterType>(static_cast<u32>(value(0)));
                const f32 frequency = value(1);
                std::array<Section, 6> sections{};
                usize count = 0;
                if (type == FilterType::LowPass || type == FilterType::HighPass) {
                    count = design_butterworth(type == FilterType::HighPass, sample_rate_, frequency, static_cast<u32>(value(4)), value(2), sections.data());
                } else {
                    sections[0] = to_section(design_biquad(type, sample_rate_, frequency, value(2), value(3)));
                    count = 1;
                }
                cascade_.set(std::span<const Section>(sections.data(), count));
            }
            Cascade cascade_;
        };

        // ================================================================================================================
        // Equalizer: eight bands
        // ================================================================================================================

        const UString kBandTypes[] = {"Off", "Low shelf", "Peak", "High shelf", "Low cut", "High cut", "Notch", "Band-pass"};
#define SFT_EQ_BAND(n, freq, type)                                                                                                                   \
    {"band" #n "_type", "", 0, 7, type, false, kBandTypes}, {"band" #n "_frequency", "Hz", 20, 20000, freq, true}, {"band" #n "_q", "", 0.1f, 18, 1.0f}, \
        {"band" #n "_gain", "dB", -24, 24, 0}
        const ParameterInfo kEqParams[] = {
            SFT_EQ_BAND(1, 60, 1),    SFT_EQ_BAND(2, 150, 2),   SFT_EQ_BAND(3, 400, 2),   SFT_EQ_BAND(4, 1000, 2),
            SFT_EQ_BAND(5, 2500, 2),  SFT_EQ_BAND(6, 6000, 2),  SFT_EQ_BAND(7, 10000, 2), SFT_EQ_BAND(8, 16000, 3),
        };
#undef SFT_EQ_BAND

        class EqualizerEffect final : public ParametricEffect {
          public:
            EqualizerEffect() : ParametricEffect(kEqParams) {
                // A freshly made equalizer is flat: the default band types would otherwise be active at 0 dB, which is a no-op
                // for shelves and peaks but burns CPU, so start with every band off until the caller picks one.
                for (u32 band = 0; band < 8; ++band) {
                    values_[band * 4] = 0.0f;
                }
            }
            [[nodiscard]] ustr name() const override { return "Equalizer"_ustr; }
            void on_prepare() override { cascade_.allocate(channels_, 8); }
            void reset() override { cascade_.reset(); }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    std::array<Section, 8> sections{};
                    usize count = 0;
                    for (u32 band = 0; band < 8; ++band) {
                        const u32 type = static_cast<u32>(value(band * 4));
                        const f32 f = value(band * 4 + 1), q = value(band * 4 + 2), g = value(band * 4 + 3);
                        FilterType ft;
                        switch (type) {
                            case 1: ft = FilterType::LowShelf; break;
                            case 2: ft = FilterType::Peak; break;
                            case 3: ft = FilterType::HighShelf; break;
                            case 4: ft = FilterType::HighPass; break;
                            case 5: ft = FilterType::LowPass; break;
                            case 6: ft = FilterType::Notch; break;
                            case 7: ft = FilterType::BandPass; break;
                            default: continue;
                        }
                        if ((ft == FilterType::Peak || ft == FilterType::LowShelf || ft == FilterType::HighShelf) && std::fabs(g) < 0.01f) {
                            continue; // a flat band does nothing
                        }
                        sections[count++] = to_section(design_biquad(ft, sample_rate_, f, q, g));
                    }
                    cascade_.set(std::span<const Section>(sections.data(), count));
                }
                cascade_.process(buffer);
            }

          private:
            Cascade cascade_;
        };

        // ================================================================================================================
        // DC blocker
        // ================================================================================================================

        const ParameterInfo kDcParams[] = {{"cutoff", "Hz", 1, 100, 10, true}};

        class DcBlockerEffect final : public ParametricEffect {
          public:
            DcBlockerEffect() : ParametricEffect(kDcParams) {}
            [[nodiscard]] ustr name() const override { return "DC blocker"_ustr; }
            void on_prepare() override { cascade_.allocate(channels_, 1); }
            void reset() override { cascade_.reset(); }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    const Section s = first_order(true, sample_rate_, value(0));
                    cascade_.set(std::span<const Section>(&s, 1));
                }
                cascade_.process(buffer);
            }

          private:
            Cascade cascade_;
        };

        // ================================================================================================================
        // Noise gate
        // ================================================================================================================

        const ParameterInfo kGateParams[] = {
            {"threshold", "dB", -100, 0, -50},     {"hysteresis", "dB", 0, 24, 4},         {"attack", "ms", 0.01f, 100, 1, true},
            {"hold", "ms", 0, 2000, 50},           {"release", "ms", 1, 5000, 150, true},  {"range", "dB", -100, 0, -80},
            {"lookahead", "ms", 0, 20, 0},         {"sidechain_highpass", "Hz", 0, 2000, 0}, {"stereo_link", "", 0, 1, 1, false, kOffOn},
        };

        class NoiseGateEffect final : public ParametricEffect {
          public:
            NoiseGateEffect() : ParametricEffect(kGateParams) {}
            [[nodiscard]] ustr name() const override { return "Noise gate"_ustr; }
            [[nodiscard]] u32 latency_frames() const override { return delay_.length(); }

            void on_prepare() override {
                delay_.allocate(channels_, static_cast<u32>(sample_rate_ * 0.02f) + 1);
                detector_filter_.allocate(channels_);
                groups_.assign(std::max(channels_, 1u), Group{});
            }
            void reset() override {
                delay_.reset();
                detector_filter_.reset();
                for (Group &g : groups_) {
                    g = Group{};
                }
            }

            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    open_threshold_ = db_to_linear(value(0));
                    close_threshold_ = db_to_linear(value(0) - value(1));
                    attack_ = time_coefficient(value(2));
                    release_ = time_coefficient(value(4));
                    hold_samples_ = static_cast<u32>(ms_to_samples(value(3)));
                    floor_ = db_to_linear(value(5));
                    delay_.set_length(static_cast<u32>(ms_to_samples(value(6))));
                    detector_filter_.set(sample_rate_, value(7));
                    linked_ = flag(8);
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const f32 level_decay = time_coefficient(5.0f);
                for (u32 i = 0; i < frames; ++i) {
                    // The detector listens to the live signal; the audio itself is delayed, so the gate opens before a transient lands.
                    f32 detected_all = 0.0f;
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 det = std::fabs(detector_filter_.process(c, buffer.data(c)[i]));
                        if (linked_) {
                            detected_all = std::max(detected_all, det);
                        } else {
                            step_group(groups_[c], det, level_decay);
                        }
                    }
                    if (linked_) {
                        step_group(groups_[0], detected_all, level_decay);
                    }
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 delayed = delay_.push_pop(c, buffer.data(c)[i]);
                        buffer.data(c)[i] = delayed * groups_[linked_ ? 0 : c].gain;
                    }
                }
            }

          private:
            struct Group {
                f32 level = 0.0f;
                f32 gain = 0.0f;
                u32 hold = 0;
                bool open = false;
            };

            void step_group(Group &g, f32 detected, f32 level_decay) const noexcept {
                g.level = detected > g.level ? detected : detected + (g.level - detected) * level_decay;
                if (!g.open) {
                    if (g.level >= open_threshold_) {
                        g.open = true;
                        g.hold = hold_samples_;
                    }
                } else if (g.level >= close_threshold_) {
                    g.hold = hold_samples_;
                } else if (g.hold > 0) {
                    --g.hold;
                } else {
                    g.open = false;
                }
                const f32 target = g.open ? 1.0f : floor_;
                const f32 coefficient = target > g.gain ? attack_ : release_;
                g.gain = target + (g.gain - target) * coefficient;
            }

            SampleDelay delay_;
            DetectorFilter detector_filter_;
            std::vector<Group> groups_;
            f32 open_threshold_ = 0.003f, close_threshold_ = 0.002f, attack_ = 0.0f, release_ = 0.0f, floor_ = 0.0f;
            u32 hold_samples_ = 0;
            bool linked_ = true;
        };

        // ================================================================================================================
        // Dynamics: shared gain-smoothing and level detection
        // ================================================================================================================

        /// Smooths a gain in dB with separate rates for falling and rising gain.
        struct GainSmoother {
            f32 current_db = 0.0f;
            [[nodiscard]] f32 step(f32 target_db, f32 fall_coefficient, f32 rise_coefficient) noexcept {
                const f32 coefficient = target_db < current_db ? fall_coefficient : rise_coefficient;
                current_db = target_db + (current_db - target_db) * coefficient;
                return current_db;
            }
        };

        // ================================================================================================================
        // Expander
        // ================================================================================================================

        const ParameterInfo kExpanderParams[] = {
            {"threshold", "dB", -100, 0, -50}, {"ratio", "", 1, 20, 2},         {"knee", "dB", 0, 24, 6},
            {"attack", "ms", 0.1f, 200, 5, true}, {"release", "ms", 5, 2000, 120, true}, {"range", "dB", -100, 0, -60},
            {"stereo_link", "", 0, 1, 1, false, kOffOn},
        };

        class ExpanderEffect final : public ParametricEffect {
          public:
            ExpanderEffect() : ParametricEffect(kExpanderParams) {}
            [[nodiscard]] ustr name() const override { return "Expander"_ustr; }
            void on_prepare() override { smoothers_.assign(std::max(channels_, 1u), GainSmoother{}); levels_.assign(std::max(channels_, 1u), 0.0f); }
            void reset() override {
                for (auto &s : smoothers_) s = GainSmoother{};
                std::fill(levels_.begin(), levels_.end(), 0.0f);
            }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    // "attack" is how fast the signal is let back in (gain rising), "release" how fast it is closed.
                    rise_ = time_coefficient(value(3));
                    fall_ = time_coefficient(value(4));
                    decay_ = time_coefficient(2.0f);
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const bool linked = flag(6);
                const f32 threshold = value(0), reduction = value(1) - 1.0f, knee = value(2), range = value(5);
                for (u32 i = 0; i < frames; ++i) {
                    f32 linked_level = 0.0f;
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 det = std::fabs(buffer.data(c)[i]);
                        levels_[c] = det > levels_[c] ? det : det + (levels_[c] - det) * decay_;
                        linked_level = std::max(linked_level, levels_[c]);
                    }
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 level_db = linear_to_db(linked ? linked_level : levels_[c]);
                        const f32 under = threshold - level_db;
                        f32 gain_db = 0.0f;
                        if (knee > 0.0f && std::fabs(under) * 2.0f <= knee) {
                            const f32 t = under + knee * 0.5f;
                            gain_db = -reduction * t * t / (2.0f * knee);
                        } else if (under > 0.0f) {
                            gain_db = -reduction * under;
                        }
                        gain_db = std::max(gain_db, range);
                        buffer.data(c)[i] *= db_to_linear(smoothers_[c].step(gain_db, fall_, rise_));
                    }
                }
            }

          private:
            std::vector<GainSmoother> smoothers_;
            std::vector<f32> levels_;
            f32 rise_ = 0.0f, fall_ = 0.0f, decay_ = 0.0f;
        };

        // ================================================================================================================
        // Compressor
        // ================================================================================================================

        const UString kDetectors[] = {"Peak", "RMS"};
        const ParameterInfo kCompressorParams[] = {
            {"threshold", "dB", -60, 0, -18},   {"ratio", "", 1, 40, 4},          {"knee", "dB", 0, 24, 6},
            {"attack", "ms", 0.01f, 250, 10, true}, {"release", "ms", 1, 2500, 100, true}, {"makeup", "dB", -12, 36, 0},
            {"auto_makeup", "", 0, 1, 0, false, kOffOn}, {"detector", "", 0, 1, 0, false, kDetectors},
            {"sidechain_highpass", "Hz", 0, 1000, 0}, {"lookahead", "ms", 0, 20, 0}, {"mix", "", 0, 1, 1},
            {"stereo_link", "", 0, 1, 1, false, kOffOn},
        };

        class CompressorEffect final : public ParametricEffect {
          public:
            CompressorEffect() : ParametricEffect(kCompressorParams) {}
            [[nodiscard]] ustr name() const override { return "Compressor"_ustr; }
            [[nodiscard]] u32 latency_frames() const override { return delay_.length(); }
            void on_prepare() override {
                delay_.allocate(channels_, static_cast<u32>(sample_rate_ * 0.02f) + 1);
                detector_filter_.allocate(channels_);
                smoothers_.assign(std::max(channels_, 1u), GainSmoother{});
                energy_.assign(std::max(channels_, 1u), 0.0f);
            }
            void reset() override {
                delay_.reset();
                detector_filter_.reset();
                for (auto &s : smoothers_) s = GainSmoother{};
                std::fill(energy_.begin(), energy_.end(), 0.0f);
            }

            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    attack_ = time_coefficient(value(3));
                    release_ = time_coefficient(value(4));
                    makeup_db_ = value(5) + (flag(6) ? -value(0) * (1.0f - 1.0f / value(1)) : 0.0f);
                    rms_ = value(7) >= 0.5f;
                    rms_decay_ = time_coefficient(10.0f);
                    detector_filter_.set(sample_rate_, value(8));
                    delay_.set_length(static_cast<u32>(ms_to_samples(value(9))));
                    mix_ = value(10);
                    linked_ = flag(11);
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const f32 threshold = value(0), ratio = value(1), knee = value(2);
                const f32 slope = 1.0f / ratio - 1.0f;
                const f32 makeup = db_to_linear(makeup_db_);
                std::array<f32, max_source_levels> level{};
                for (u32 i = 0; i < frames; ++i) {
                    f32 linked_level = 0.0f;
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 det = detector_filter_.process(c, buffer.data(c)[i]);
                        f32 amount;
                        if (rms_) {
                            energy_[c] = det * det + (energy_[c] - det * det) * rms_decay_;
                            amount = std::sqrt(energy_[c]);
                        } else {
                            amount = std::fabs(det);
                        }
                        level[c % max_source_levels] = amount;
                        linked_level = std::max(linked_level, amount);
                    }
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 level_db = linear_to_db(linked_ ? linked_level : level[c % max_source_levels]);
                        const f32 over = level_db - threshold;
                        f32 reduction_db = 0.0f;
                        if (knee > 0.0f && std::fabs(over) * 2.0f <= knee) {
                            const f32 t = over + knee * 0.5f;
                            reduction_db = slope * t * t / (2.0f * knee);
                        } else if (over > 0.0f) {
                            reduction_db = slope * over;
                        }
                        // Attack when the reduction deepens, release when it recovers.
                        const f32 smoothed = smoothers_[c].step(reduction_db, attack_, release_);
                        const f32 dry = delay_.push_pop(c, buffer.data(c)[i]);
                        const f32 wet = dry * db_to_linear(smoothed) * makeup;
                        buffer.data(c)[i] = dry + (wet - dry) * mix_;
                    }
                }
            }

          private:
            static constexpr u32 max_source_levels = 64;
            SampleDelay delay_;
            DetectorFilter detector_filter_;
            std::vector<GainSmoother> smoothers_;
            std::vector<f32> energy_;
            f32 attack_ = 0.0f, release_ = 0.0f, makeup_db_ = 0.0f, rms_decay_ = 0.0f, mix_ = 1.0f;
            bool rms_ = false, linked_ = true;
        };

        // ================================================================================================================
        // Look-ahead limiter
        // ================================================================================================================

        const ParameterInfo kLimiterParams[] = {
            {"ceiling", "dB", -24, 0, -0.3f}, {"release", "ms", 1, 1000, 80, true}, {"lookahead", "ms", 0.1f, 10, 1.5f},
        };

        class LimiterEffect final : public ParametricEffect {
          public:
            LimiterEffect() : ParametricEffect(kLimiterParams) {}
            [[nodiscard]] ustr name() const override { return "Limiter"_ustr; }
            [[nodiscard]] u32 latency_frames() const override { return lookahead_; }

            void on_prepare() override {
                max_lookahead_ = static_cast<u32>(sample_rate_ * 0.010f) + 2;
                delay_.allocate(channels_, max_lookahead_);
                min_value_.assign(max_lookahead_ + 2, 1.0f);
                min_index_.assign(max_lookahead_ + 2, 0);
                smooth_ring_.assign(max_lookahead_ + 2, 1.0f);
                lookahead_ = 0;
            }
            void reset() override {
                delay_.reset();
                std::fill(min_value_.begin(), min_value_.end(), 1.0f);
                std::fill(smooth_ring_.begin(), smooth_ring_.end(), 1.0f);
                head_ = tail_ = 0;
                counter_ = 0;
                smooth_pos_ = 0;
                smooth_sum_ = 0.0;
                gain_ = 1.0f;
                restart_ = true;
                dirty_ = true;
            }

            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    ceiling_ = db_to_linear(value(0));
                    release_ = time_coefficient(value(1));
                    const u32 wanted = std::clamp(static_cast<u32>(ms_to_samples(value(2))), 1u, max_lookahead_);
                    if (wanted != lookahead_ || restart_) {
                        lookahead_ = wanted;
                        delay_.set_length(lookahead_);
                        // The windows depend on the length: start them fresh at unity gain.
                        std::fill(min_value_.begin(), min_value_.end(), 1.0f);
                        std::fill(smooth_ring_.begin(), smooth_ring_.end(), 1.0f);
                        head_ = tail_ = 0;
                        counter_ = 0;
                        smooth_pos_ = 0;
                        smooth_sum_ = static_cast<f64>(lookahead_ + 1);
                        restart_ = false;
                    }
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const u32 window = lookahead_ + 1;
                const u32 capacity = static_cast<u32>(min_value_.size());
                for (u32 i = 0; i < frames; ++i) {
                    f32 peak = 0.0f;
                    for (u32 c = 0; c < channels; ++c) {
                        peak = std::max(peak, std::fabs(buffer.data(c)[i]));
                    }
                    const f32 needed = peak > ceiling_ ? ceiling_ / peak : 1.0f;
                    // Sliding-window minimum of `needed` over the last `window` samples (monotonic deque in a ring).
                    while (tail_ != head_) {
                        const u32 back = tail_ == 0 ? capacity - 1 : tail_ - 1;
                        if (min_value_[back] >= needed) {
                            tail_ = back;
                        } else {
                            break;
                        }
                    }
                    min_value_[tail_] = needed;
                    min_index_[tail_] = counter_;
                    tail_ = tail_ + 1 == capacity ? 0 : tail_ + 1;
                    while (counter_ - min_index_[head_] >= window) {
                        head_ = head_ + 1 == capacity ? 0 : head_ + 1;
                    }
                    const f32 window_min = min_value_[head_];
                    ++counter_;
                    // Average the minima over the same window so gain ramps down across the look-ahead instead of stepping.
                    smooth_sum_ += static_cast<f64>(window_min) - static_cast<f64>(smooth_ring_[smooth_pos_]);
                    smooth_ring_[smooth_pos_] = window_min;
                    smooth_pos_ = smooth_pos_ + 1 == window ? 0 : smooth_pos_ + 1;
                    const f32 target = static_cast<f32>(smooth_sum_ / static_cast<f64>(window));
                    // Instant when more reduction is needed, slow recovery otherwise; never above what this sample allows.
                    gain_ = std::min(target, target < gain_ ? target : gain_ + (target - gain_) * (1.0f - release_));
                    for (u32 c = 0; c < channels; ++c) {
                        buffer.data(c)[i] = delay_.push_pop(c, buffer.data(c)[i]) * gain_;
                    }
                }
            }

          private:
            SampleDelay delay_;
            std::vector<f32> min_value_;
            std::vector<u32> min_index_;
            std::vector<f32> smooth_ring_;
            u32 head_ = 0, tail_ = 0, counter_ = 0, smooth_pos_ = 0;
            f64 smooth_sum_ = 0.0;
            u32 lookahead_ = 0, max_lookahead_ = 0;
            f32 ceiling_ = 0.97f, release_ = 0.0f, gain_ = 1.0f;
            bool restart_ = true;
        };

        // ================================================================================================================
        // De-esser
        // ================================================================================================================

        const ParameterInfo kDeEsserParams[] = {
            {"frequency", "Hz", 2000, 12000, 6500, true}, {"q", "", 0.5f, 5, 1.5f}, {"threshold", "dB", -60, 0, -30},
            {"ratio", "", 1, 20, 4},          {"attack", "ms", 0.1f, 20, 0.5f},  {"release", "ms", 5, 500, 50},
        };

        class DeEsserEffect final : public ParametricEffect {
          public:
            DeEsserEffect() : ParametricEffect(kDeEsserParams) {}
            [[nodiscard]] ustr name() const override { return "De-esser"_ustr; }
            void on_prepare() override {
                band_.assign(std::max(channels_, 1u) * 2, 0.0f);
                level_ = 0.0f;
                smoother_ = GainSmoother{};
            }
            void reset() override {
                std::fill(band_.begin(), band_.end(), 0.0f);
                level_ = 0.0f;
                smoother_ = GainSmoother{};
            }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    section_ = to_section(design_biquad(FilterType::BandPass, sample_rate_, value(0), value(1)));
                    attack_ = time_coefficient(value(4));
                    release_ = time_coefficient(value(5));
                    decay_ = time_coefficient(3.0f);
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const f32 slope = 1.0f - 1.0f / value(3);
                std::array<f32, 64> band_now{};
                for (u32 i = 0; i < frames; ++i) {
                    f32 detected = 0.0f;
                    for (u32 c = 0; c < channels && c < band_now.size(); ++c) {
                        const f32 in = buffer.data(c)[i];
                        f32 &z1 = band_[c * 2], &z2 = band_[c * 2 + 1];
                        const f32 out = section_.b0 * in + z1;
                        z1 = section_.b1 * in - section_.a1 * out + z2;
                        z2 = section_.b2 * in - section_.a2 * out;
                        band_now[c] = out;
                        detected = std::max(detected, std::fabs(out));
                    }
                    level_ = detected > level_ ? detected : detected + (level_ - detected) * decay_;
                    const f32 over = linear_to_db(level_) - value(2);
                    const f32 reduction_db = over > 0.0f ? -over * slope : 0.0f;
                    const f32 g = db_to_linear(smoother_.step(reduction_db, attack_, release_));
                    for (u32 c = 0; c < channels && c < band_now.size(); ++c) {
                        // Only the sibilant band is turned down; everything else passes untouched.
                        buffer.data(c)[i] -= band_now[c] * (1.0f - g);
                    }
                }
            }

          private:
            Section section_;
            std::vector<f32> band_;
            f32 level_ = 0.0f, attack_ = 0.0f, release_ = 0.0f, decay_ = 0.0f;
            GainSmoother smoother_;
        };

        // ================================================================================================================
        // Hum filter
        // ================================================================================================================

        const ParameterInfo kHumParams[] = {{"base_frequency", "Hz", 20, 400, 50}, {"harmonics", "", 1, 16, 4}, {"q", "", 5, 100, 30}};

        class HumFilterEffect final : public ParametricEffect {
          public:
            HumFilterEffect() : ParametricEffect(kHumParams) {}
            [[nodiscard]] ustr name() const override { return "Hum filter"_ustr; }
            void on_prepare() override { cascade_.allocate(channels_, 16); }
            void reset() override { cascade_.reset(); }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    std::array<Section, 16> sections{};
                    usize count = 0;
                    for (u32 h = 1; h <= static_cast<u32>(value(1)); ++h) {
                        const f32 f = value(0) * static_cast<f32>(h);
                        if (f >= sample_rate_ * 0.45f) {
                            break;
                        }
                        sections[count++] = to_section(design_biquad(FilterType::Notch, sample_rate_, f, value(2)));
                    }
                    cascade_.set(std::span<const Section>(sections.data(), count));
                }
                cascade_.process(buffer);
            }

          private:
            Cascade cascade_;
        };

        // ================================================================================================================
        // Delay
        // ================================================================================================================

        const ParameterInfo kDelayParams[] = {
            {"time", "ms", 1, 2000, 250, true}, {"feedback", "", 0, 0.95f, 0.35f}, {"damping", "Hz", 200, 20000, 8000, true},
            {"mix", "", 0, 1, 0.3f},            {"ping_pong", "", 0, 1, 0, false, kOffOn},
        };

        class DelayEffect final : public ParametricEffect {
          public:
            DelayEffect() : ParametricEffect(kDelayParams) {}
            [[nodiscard]] ustr name() const override { return "Delay"_ustr; }
            [[nodiscard]] u32 tail_frames() const override {
                const f32 feedback = std::max(value_of(1), 0.05f);
                const f32 repeats = std::min(std::log(0.001f) / std::log(feedback), 200.0f);
                return static_cast<u32>(std::min(value_of(0) * 0.001f * sample_rate_ * repeats, sample_rate_ * 10.0f));
            }
            void on_prepare() override {
                lines_.assign(std::max(channels_, 1u), DelayLine{});
                for (auto &l : lines_) l.resize(static_cast<u32>(sample_rate_ * 2.1f));
                damp_.assign(std::max(channels_, 1u), 0.0f);
                time_current_ = value(0);
            }
            void reset() override {
                for (auto &l : lines_) l.clear();
                std::fill(damp_.begin(), damp_.end(), 0.0f);
            }
            void process(AudioBuffer &buffer) override {
                if (take_dirty()) {
                    damp_coefficient_ = 1.0f - std::exp(-2.0f * kPi * value(2) / sample_rate_);
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const f32 feedback = value(1), mix = value(3);
                const bool ping_pong = flag(4) && channels >= 2;
                // Glide toward the requested time so moving the control does not click (it will pitch-bend, like tape).
                time_current_ += (value(0) - time_current_) * 0.08f;
                const f32 delay_samples = std::max(time_current_ * 0.001f * sample_rate_, 1.0f);
                for (u32 i = 0; i < frames; ++i) {
                    std::array<f32, 64> delayed{};
                    for (u32 c = 0; c < channels && c < delayed.size(); ++c) {
                        delayed[c] = lines_[c].read(delay_samples);
                        damp_[c] += (delayed[c] - damp_[c]) * damp_coefficient_;
                    }
                    for (u32 c = 0; c < channels && c < delayed.size(); ++c) {
                        const f32 dry = buffer.data(c)[i];
                        const f32 fed = ping_pong && c < 2 ? damp_[1 - c] : damp_[c];
                        const f32 in = ping_pong && c == 0 ? 0.5f * (dry + (channels > 1 ? buffer.data(1)[i] : dry)) : (ping_pong && c == 1 ? 0.0f : dry);
                        lines_[c].write(in + fed * feedback);
                        buffer.data(c)[i] = dry * (1.0f - mix) + delayed[c] * mix;
                    }
                }
            }

          private:
            [[nodiscard]] f32 value_of(u32 index) const noexcept { return values_[index]; }
            std::vector<DelayLine> lines_;
            std::vector<f32> damp_;
            f32 damp_coefficient_ = 1.0f;
            f32 time_current_ = 250.0f;
        };

        // ================================================================================================================
        // Modulation: chorus, flanger, phaser, tremolo, vibrato
        // ================================================================================================================

        const UString kModKinds[] = {"Chorus", "Flanger", "Phaser", "Tremolo", "Vibrato"};
        const ParameterInfo kModParams[] = {
            {"kind", "", 0, 4, 0, false, kModKinds}, {"rate", "Hz", 0.01f, 20, 0.8f, true}, {"depth", "", 0, 1, 0.5f},
            {"feedback", "", -0.95f, 0.95f, 0}, {"mix", "", 0, 1, 0.5f}, {"stereo_phase", "", 0, 1, 0.25f},
        };

        class ModulationEffect final : public ParametricEffect {
          public:
            ModulationEffect() : ParametricEffect(kModParams) {}
            [[nodiscard]] ustr name() const override { return "Modulation"_ustr; }
            void on_prepare() override {
                lines_.assign(std::max(channels_, 1u), DelayLine{});
                for (auto &l : lines_) l.resize(static_cast<u32>(sample_rate_ * 0.06f));
                allpass_.assign(std::max(channels_, 1u) * kStages, 0.0f);
                feedback_state_.assign(std::max(channels_, 1u), 0.0f);
                phase_ = 0.0f;
            }
            void reset() override {
                for (auto &l : lines_) l.clear();
                std::fill(allpass_.begin(), allpass_.end(), 0.0f);
                std::fill(feedback_state_.begin(), feedback_state_.end(), 0.0f);
            }
            void process(AudioBuffer &buffer) override {
                (void)take_dirty();
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const u32 kind = static_cast<u32>(value(0));
                const f32 depth = value(2), feedback = value(3), mix = value(4), spread = value(5);
                const f32 increment = value(1) / sample_rate_;
                for (u32 i = 0; i < frames; ++i) {
                    for (u32 c = 0; c < channels; ++c) {
                        f32 phase = phase_ + spread * 0.5f * static_cast<f32>(c);
                        phase -= std::floor(phase);
                        const f32 lfo = std::sin(2.0f * kPi * phase); // -1..1
                        const f32 dry = buffer.data(c)[i];
                        f32 out = dry;
                        switch (kind) {
                            case 0: { // chorus: ~20 ms centre, up to +-8 ms swing
                                const f32 delay = (0.020f + 0.008f * depth * lfo) * sample_rate_;
                                const f32 wet = lines_[c].read(delay);
                                lines_[c].write(dry);
                                out = dry * (1.0f - mix) + wet * mix;
                                break;
                            }
                            case 1: { // flanger: ~2.5 ms centre, resonant feedback
                                const f32 delay = (0.0025f + 0.0020f * depth * lfo) * sample_rate_;
                                const f32 wet = lines_[c].read(delay);
                                lines_[c].write(dry + wet * feedback);
                                out = dry * (1.0f - mix) + wet * mix;
                                break;
                            }
                            case 2: { // phaser: swept first-order all-pass chain
                                const f32 frequency = 300.0f * std::pow(10.0f, depth * 1.2f * (0.5f + 0.5f * lfo));
                                const f32 tan_half = std::tan(kPi * std::min(frequency, sample_rate_ * 0.45f) / sample_rate_);
                                const f32 a = (tan_half - 1.0f) / (tan_half + 1.0f);
                                f32 x = dry + feedback_state_[c] * feedback;
                                for (u32 s = 0; s < kStages; ++s) {
                                    f32 &z = allpass_[c * kStages + s];
                                    const f32 y = a * x + z;
                                    z = x - a * y;
                                    x = y;
                                }
                                feedback_state_[c] = x;
                                out = dry * (1.0f - mix) + x * mix;
                                break;
                            }
                            case 3: // tremolo
                                out = dry * (1.0f - depth * (0.5f + 0.5f * lfo));
                                break;
                            default: { // vibrato: pitch wobble, fully wet
                                const f32 delay = (0.004f + 0.003f * depth * lfo) * sample_rate_;
                                lines_[c].write(dry);
                                out = lines_[c].read(delay);
                                break;
                            }
                        }
                        buffer.data(c)[i] = out;
                    }
                    phase_ += increment;
                    phase_ -= std::floor(phase_);
                }
            }

          private:
            static constexpr u32 kStages = 6;
            std::vector<DelayLine> lines_;
            std::vector<f32> allpass_, feedback_state_;
            f32 phase_ = 0.0f;
        };

        // ================================================================================================================
        // Distortion
        // ================================================================================================================

        const UString kDistortionKinds[] = {"Soft clip", "Hard clip", "Wavefold", "Bit crush"};
        const ParameterInfo kDistortionParams[] = {
            {"kind", "", 0, 3, 0, false, kDistortionKinds}, {"drive", "dB", 0, 48, 12}, {"bits", "", 1, 24, 8},
            {"downsample", "", 1, 64, 1}, {"output", "dB", -36, 12, -6}, {"mix", "", 0, 1, 1},
        };

        class DistortionEffect final : public ParametricEffect {
          public:
            DistortionEffect() : ParametricEffect(kDistortionParams) {}
            [[nodiscard]] ustr name() const override { return "Distortion"_ustr; }
            void on_prepare() override {
                held_.assign(std::max(channels_, 1u), 0.0f);
                counter_ = 0;
            }
            void process(AudioBuffer &buffer) override {
                (void)take_dirty();
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const u32 kind = static_cast<u32>(value(0));
                const f32 drive = db_to_linear(value(1)), output = db_to_linear(value(4)), mix = value(5);
                const f32 step = std::ldexp(2.0f, -static_cast<int>(value(2))); // quantiser step for `bits` bits over [-1, 1]
                const u32 hold = static_cast<u32>(value(3));
                for (u32 i = 0; i < frames; ++i) {
                    const bool sample_now = counter_ % std::max(hold, 1u) == 0;
                    for (u32 c = 0; c < channels; ++c) {
                        const f32 dry = buffer.data(c)[i];
                        const f32 driven = dry * drive;
                        f32 wet;
                        switch (kind) {
                            case 0: wet = std::tanh(driven); break;
                            case 1: wet = std::clamp(driven, -1.0f, 1.0f); break;
                            case 2: wet = std::sin(driven * kPi * 0.5f); break;
                            default:
                                if (sample_now) {
                                    held_[c] = std::round(std::clamp(driven, -1.0f, 1.0f) / step) * step;
                                }
                                wet = held_[c];
                                break;
                        }
                        buffer.data(c)[i] = (dry * (1.0f - mix) + wet * mix * output);
                    }
                    ++counter_;
                }
            }

          private:
            std::vector<f32> held_;
            u32 counter_ = 0;
        };

        // ================================================================================================================
        // Stereo width, gain/pan
        // ================================================================================================================

        const ParameterInfo kWidthParams[] = {{"width", "", 0, 2, 1}, {"balance", "", -1, 1, 0}};

        class StereoWidthEffect final : public ParametricEffect {
          public:
            StereoWidthEffect() : ParametricEffect(kWidthParams) {}
            [[nodiscard]] ustr name() const override { return "Stereo width"_ustr; }
            void process(AudioBuffer &buffer) override {
                (void)take_dirty();
                if (buffer.channels() < 2) {
                    return;
                }
                const f32 width = value(0), balance = value(1);
                const f32 left_gain = std::min(1.0f, 1.0f - balance), right_gain = std::min(1.0f, 1.0f + balance);
                f32 *l = buffer.data(0);
                f32 *r = buffer.data(1);
                for (u32 i = 0; i < buffer.frames(); ++i) {
                    const f32 mid = 0.5f * (l[i] + r[i]);
                    const f32 side = 0.5f * (l[i] - r[i]) * width;
                    l[i] = (mid + side) * left_gain;
                    r[i] = (mid - side) * right_gain;
                }
            }
        };

        const ParameterInfo kGainPanParams[] = {
            {"gain", "dB", -96, 24, 0}, {"pan", "", -1, 1, 0}, {"invert_polarity", "", 0, 1, 0, false, kOffOn}, {"mono", "", 0, 1, 0, false, kOffOn},
        };

        class GainPanEffect final : public ParametricEffect {
          public:
            GainPanEffect() : ParametricEffect(kGainPanParams) {}
            [[nodiscard]] ustr name() const override { return "Gain / pan"_ustr; }
            void on_prepare() override { previous_.assign(std::max(channels_, 1u), -1.0f); }
            void process(AudioBuffer &buffer) override {
                (void)take_dirty();
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                if (flag(3) && channels >= 2) {
                    for (u32 i = 0; i < frames; ++i) {
                        f32 sum = 0.0f;
                        for (u32 c = 0; c < channels; ++c) sum += buffer.data(c)[i];
                        sum /= static_cast<f32>(channels);
                        for (u32 c = 0; c < channels; ++c) buffer.data(c)[i] = sum;
                    }
                }
                const f32 base = db_to_linear(value(0)) * (flag(2) ? -1.0f : 1.0f);
                const f32 pan = value(1);
                for (u32 c = 0; c < channels; ++c) {
                    f32 gain = base;
                    if (channels >= 2 && c == 0) gain *= std::min(1.0f, 1.0f - pan);
                    if (channels >= 2 && c == 1) gain *= std::min(1.0f, 1.0f + pan);
                    const f32 from = previous_[c] == -1.0f ? gain : previous_[c];
                    Kernels::scale_ramp(buffer.channel(c).first(std::min<usize>(frames, buffer.frames())), from, gain);
                    previous_[c] = gain;
                }
            }

          private:
            std::vector<f32> previous_;
        };

        // ================================================================================================================
        // Convolution
        // ================================================================================================================

        const ParameterInfo kConvolutionParams[] = {{"wet", "", 0, 1, 1}, {"dry", "", 0, 1, 0}, {"gain", "dB", -24, 24, 0}};

        class ConvolutionEffect final : public ParametricEffect {
          public:
            ConvolutionEffect(std::shared_ptr<const SampleBuffer> impulse, f32 wet, f32 dry, f32 gain_db, f32 predelay_ms)
                : ParametricEffect(kConvolutionParams), impulse_(std::move(impulse)), predelay_ms_(predelay_ms) {
                values_[0] = wet;
                values_[1] = dry;
                values_[2] = gain_db;
            }
            [[nodiscard]] ustr name() const override { return "Convolution"_ustr; }
            [[nodiscard]] u32 latency_frames() const override { return staging_ ? partition_ : 0; }
            [[nodiscard]] u32 tail_frames() const override { return ir_frames_; }

            void on_prepare() override {
                partition_ = 64;
                while (partition_ < max_frames_ && partition_ < 8192) partition_ <<= 1;
                fft_ = std::make_unique<Fft>(partition_ * 2);
                bins_ = partition_ + 1;
                build_impulse();
                const u32 channels = std::max(channels_, 1u);
                inputs_.assign(channels, {});
                for (auto &in : inputs_) {
                    in.history_re.assign(static_cast<usize>(partitions_) * bins_, 0.0f);
                    in.history_im.assign(static_cast<usize>(partitions_) * bins_, 0.0f);
                    in.previous.assign(partition_, 0.0f);
                    in.queue.assign(partition_ * 2, 0.0f);
                    in.pending.assign(partition_, 0.0f);
                }
                time_.assign(partition_ * 2, 0.0f);
                wet_.assign(partition_, 0.0f);
                scratch_.assign(fft_->real_scratch_size(), 0.0f);
                acc_re_.assign(bins_, 0.0f);
                acc_im_.assign(bins_, 0.0f);
                spec_re_.assign(bins_, 0.0f);
                spec_im_.assign(bins_, 0.0f);
                history_position_ = 0;
                fill_ = 0;
                queue_read_ = 0;
                queue_count_ = 0;
                staging_ = max_frames_ != partition_;
            }
            void reset() override {
                for (auto &in : inputs_) {
                    std::fill(in.history_re.begin(), in.history_re.end(), 0.0f);
                    std::fill(in.history_im.begin(), in.history_im.end(), 0.0f);
                    std::fill(in.previous.begin(), in.previous.end(), 0.0f);
                    std::fill(in.queue.begin(), in.queue.end(), 0.0f);
                }
                history_position_ = 0;
                fill_ = 0;
                queue_read_ = 0;
                queue_count_ = staging_ ? partition_ : 0;
            }

            void process(AudioBuffer &buffer) override {
                (void)take_dirty();
                if (!fft_ || impulse_spectra_.empty()) {
                    return;
                }
                const u32 channels = active_channels(buffer);
                const u32 frames = buffer.frames();
                const f32 wet = value(0) * db_to_linear(value(2)), dry = value(1);
                if (frames == partition_ && !staging_) {
                    for (u32 c = 0; c < channels; ++c) {
                        f32 *samples = buffer.data(c);
                        convolve_block(c, samples, wet_.data());
                        for (u32 i = 0; i < partition_; ++i) {
                            samples[i] = wet_[i] * wet + samples[i] * dry;
                        }
                    }
                    advance_history();
                    return;
                }
                // Odd block sizes: collect input into partitions, emit output one partition late.
                for (u32 i = 0; i < frames; ++i) {
                    for (u32 c = 0; c < channels; ++c) {
                        inputs_[c].pending[fill_] = buffer.data(c)[i];
                    }
                    ++fill_;
                    if (fill_ == partition_) {
                        for (u32 c = 0; c < channels; ++c) {
                            convolve_block(c, inputs_[c].pending.data(), wet_.data());
                            Input &in = inputs_[c];
                            for (u32 k = 0; k < partition_; ++k) {
                                const usize slot = (queue_read_ + queue_count_ + k) % (partition_ * 2);
                                in.queue[slot] = wet_[k] * wet + in.pending[k] * dry;
                            }
                        }
                        advance_history();
                        queue_count_ += partition_;
                        fill_ = 0;
                    }
                    for (u32 c = 0; c < channels; ++c) {
                        buffer.data(c)[i] = queue_count_ > 0 ? inputs_[c].queue[queue_read_] : 0.0f;
                    }
                    if (queue_count_ > 0) {
                        queue_read_ = (queue_read_ + 1) % (partition_ * 2);
                        --queue_count_;
                    }
                }
            }

          private:
            struct Input {
                std::vector<f32> history_re, history_im; // frequency-domain input history, `partitions_` x `bins_`
                std::vector<f32> previous;               // the previous input partition (overlap-save)
                std::vector<f32> pending;                // staging path: input being collected
                std::vector<f32> queue;                  // staging path: finished output
            };

            void build_impulse() {
                impulse_spectra_.clear();
                ir_channels_ = 0;
                partitions_ = 1;
                ir_frames_ = 0;
                if (!impulse_ || !impulse_->samples || impulse_->channels == 0 || impulse_->frames() == 0) {
                    return;
                }
                std::shared_ptr<const SampleBuffer> ir = impulse_;
                std::shared_ptr<SampleBuffer> converted;
                if (static_cast<f32>(impulse_->sample_rate) != sample_rate_) {
                    converted = resample(*impulse_, static_cast<u32>(sample_rate_));
                    ir = converted;
                }
                ir_channels_ = ir->channels;
                const u32 predelay = static_cast<u32>(ms_to_samples(predelay_ms_));
                const u32 frames = static_cast<u32>(ir->frames()) + predelay;
                ir_frames_ = frames;
                partitions_ = std::max(1u, (frames + partition_ - 1) / partition_);
                impulse_spectra_.assign(ir_channels_, {});
                std::vector<f32> block(partition_ * 2);
                for (u32 c = 0; c < ir_channels_; ++c) {
                    auto &spectra = impulse_spectra_[c];
                    spectra.re.assign(static_cast<usize>(partitions_) * bins_, 0.0f);
                    spectra.im.assign(static_cast<usize>(partitions_) * bins_, 0.0f);
                    for (u32 p = 0; p < partitions_; ++p) {
                        std::fill(block.begin(), block.end(), 0.0f);
                        for (u32 i = 0; i < partition_; ++i) {
                            const i64 index = static_cast<i64>(p) * partition_ + i - predelay;
                            if (index >= 0 && static_cast<u64>(index) < ir->frames()) {
                                block[i] = (*ir->samples)[static_cast<usize>(index) * ir_channels_ + c];
                            }
                        }
                        fft_->forward_real(block.data(), spectra.re.data() + static_cast<usize>(p) * bins_,
                                           spectra.im.data() + static_cast<usize>(p) * bins_, scratch_tmp(fft_->real_scratch_size()));
                    }
                }
            }
            f32 *scratch_tmp(usize size) {
                if (build_scratch_.size() < size) build_scratch_.resize(size);
                return build_scratch_.data();
            }

            // Convolves one partition: `in` (partition_ samples) -> `out` (partition_ samples); `out` must not be `time_`.
            void convolve_block(u32 channel, const f32 *in, f32 *out) {
                Input &state = inputs_[channel];
                std::copy_n(state.previous.begin(), partition_, time_.begin());
                std::copy_n(in, partition_, time_.begin() + partition_);
                std::copy_n(in, partition_, state.previous.begin());
                f32 *slot_re = state.history_re.data() + static_cast<usize>(history_position_) * bins_;
                f32 *slot_im = state.history_im.data() + static_cast<usize>(history_position_) * bins_;
                fft_->forward_real(time_.data(), slot_re, slot_im, scratch_.data());
                std::fill(acc_re_.begin(), acc_re_.end(), 0.0f);
                std::fill(acc_im_.begin(), acc_im_.end(), 0.0f);
                const auto &ir = impulse_spectra_[channel % ir_channels_];
                for (u32 p = 0; p < partitions_; ++p) {
                    const u32 slot = (history_position_ + partitions_ - p) % partitions_;
                    const f32 *__restrict xr = state.history_re.data() + static_cast<usize>(slot) * bins_;
                    const f32 *__restrict xi = state.history_im.data() + static_cast<usize>(slot) * bins_;
                    const f32 *__restrict hr = ir.re.data() + static_cast<usize>(p) * bins_;
                    const f32 *__restrict hi = ir.im.data() + static_cast<usize>(p) * bins_;
                    f32 *__restrict ar = acc_re_.data();
                    f32 *__restrict ai = acc_im_.data();
                    for (u32 b = 0; b < bins_; ++b) {
                        ar[b] += xr[b] * hr[b] - xi[b] * hi[b];
                        ai[b] += xr[b] * hi[b] + xi[b] * hr[b];
                    }
                }
                fft_->inverse_real(acc_re_.data(), acc_im_.data(), time_.data(), scratch_.data());
                std::copy_n(time_.begin() + partition_, partition_, out); // overlap-save: the second half is valid
            }
            void advance_history() { history_position_ = (history_position_ + 1) % partitions_; }

            struct Spectra {
                std::vector<f32> re, im;
            };

            std::shared_ptr<const SampleBuffer> impulse_;
            f32 predelay_ms_ = 0.0f;
            std::unique_ptr<Fft> fft_;
            u32 partition_ = 256, bins_ = 257, partitions_ = 1, ir_channels_ = 0, ir_frames_ = 0;
            std::vector<Spectra> impulse_spectra_;
            std::vector<Input> inputs_;
            std::vector<f32> time_, wet_, scratch_, acc_re_, acc_im_, spec_re_, spec_im_, build_scratch_;
            u32 history_position_ = 0;
            bool staging_ = false;
            u32 fill_ = 0;
            usize queue_read_ = 0, queue_count_ = 0;
        };

        // ---- registry ---------------------------------------------------------------------------------------------------

        struct KindEntry {
            EffectKind kind;
            UString name;
            std::span<const ParameterInfo> parameters;
            std::unique_ptr<AudioEffect> (*create)();
        };

        template <typename T>
        std::unique_ptr<AudioEffect> create() {
            return std::make_unique<T>();
        }

        const std::array<KindEntry, 15> &registry() {
            static const std::array<KindEntry, 15> entries{{
                {EffectKind::Filter, "Filter", kFilterParams, create<FilterEffect>},
                {EffectKind::Equalizer, "Equalizer", kEqParams, create<EqualizerEffect>},
                {EffectKind::DcBlocker, "DC blocker", kDcParams, create<DcBlockerEffect>},
                {EffectKind::NoiseGate, "Noise gate", kGateParams, create<NoiseGateEffect>},
                {EffectKind::Expander, "Expander", kExpanderParams, create<ExpanderEffect>},
                {EffectKind::Compressor, "Compressor", kCompressorParams, create<CompressorEffect>},
                {EffectKind::Limiter, "Limiter", kLimiterParams, create<LimiterEffect>},
                {EffectKind::DeEsser, "De-esser", kDeEsserParams, create<DeEsserEffect>},
                {EffectKind::HumFilter, "Hum filter", kHumParams, create<HumFilterEffect>},
                {EffectKind::Delay, "Delay", kDelayParams, create<DelayEffect>},
                {EffectKind::Modulation, "Modulation", kModParams, create<ModulationEffect>},
                {EffectKind::Distortion, "Distortion", kDistortionParams, create<DistortionEffect>},
                {EffectKind::StereoWidth, "Stereo width", kWidthParams, create<StereoWidthEffect>},
                {EffectKind::GainPan, "Gain / pan", kGainPanParams, create<GainPanEffect>},
                {EffectKind::NoiseReduction, "Noise reduction", noise_reduction_parameters(), [] { return std::unique_ptr<AudioEffect>(make_noise_reduction()); }},
            }};
            return entries;
        }

        const KindEntry &entry_for(EffectKind kind) {
            for (const KindEntry &e : registry()) {
                if (e.kind == kind) {
                    return e;
                }
            }
            return registry().front();
        }

        std::unique_ptr<AudioEffect> with(std::unique_ptr<AudioEffect> effect, std::initializer_list<std::pair<UString, f32>> values) {
            for (const auto &[name, v] : values) {
                effect->set_parameter(name, v);
            }
            return effect;
        }

    } // namespace

    // ---- public API ---------------------------------------------------------------------------------------------------------

    std::expected<std::unique_ptr<AudioEffect>, UString> make_effect(const EffectSpec &spec) {
        const KindEntry &entry = entry_for(spec.kind);
        auto effect = entry.create();
        for (const auto &[name, value] : spec.values) {
            if (!effect->set_parameter(name, value)) {
                UString valid;
                for (const ParameterInfo &info : entry.parameters) {
                    valid += valid.empty() ? UString{} : UString{", "};
                    valid += info.name;
                }
                return std::unexpected(UString{std::format("effect '{}' has no parameter '{}' (valid: {})", entry.name, name, valid)});
            }
        }
        return effect;
    }

    std::expected<std::unique_ptr<AudioEffect>, UString> make_effect(const EffectSpec &spec, f32 sample_rate, u32 channels, u32 max_frames) {
        auto effect = make_effect(spec);
        if (effect) {
            (*effect)->prepare(sample_rate, channels, max_frames);
        }
        return effect;
    }

    ustr effect_kind_name(EffectKind kind) noexcept { return ustr{entry_for(kind).name.cpp_string_view()}; }
    std::string_view effect_kind_token(EffectKind kind) {
        static const std::vector<std::pair<EffectKind, std::string>> tokens = [] {
            std::vector<std::pair<EffectKind, std::string>> out;
            for (const KindEntry &e : registry()) {
                std::string token;
                for (const char c : e.name.cpp_string_view()) {
                    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                    if (alnum) {
                        token.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
                    } else if (!token.empty() && token.back() != '_') {
                        token.push_back('_');
                    }
                }
                while (!token.empty() && token.back() == '_') token.pop_back();
                out.emplace_back(e.kind, std::move(token));
            }
            return out;
        }();
        for (const auto &[k, token] : tokens) {
            if (k == kind) return token;
        }
        return {};
    }
    std::span<const ParameterInfo> effect_parameters(EffectKind kind) { return entry_for(kind).parameters; }
    std::vector<EffectKind> all_effect_kinds() {
        std::vector<EffectKind> kinds;
        for (const KindEntry &e : registry()) {
            kinds.push_back(e.kind);
        }
        return kinds;
    }

    std::unique_ptr<AudioEffect> make_low_pass(f32 cutoff_hz, u32 order, f32 q) {
        return with(create<FilterEffect>(), {{"type", 0}, {"frequency", cutoff_hz}, {"order", static_cast<f32>(order)}, {"q", q}});
    }
    std::unique_ptr<AudioEffect> make_high_pass(f32 cutoff_hz, u32 order, f32 q) {
        return with(create<FilterEffect>(), {{"type", 1}, {"frequency", cutoff_hz}, {"order", static_cast<f32>(order)}, {"q", q}});
    }
    std::unique_ptr<AudioEffect> make_band_pass(f32 center_hz, f32 q) {
        return with(create<FilterEffect>(), {{"type", 2}, {"frequency", center_hz}, {"q", q}});
    }
    std::unique_ptr<AudioEffect> make_noise_gate(f32 threshold_db, f32 attack_ms, f32 hold_ms, f32 release_ms) {
        return with(create<NoiseGateEffect>(), {{"threshold", threshold_db}, {"attack", attack_ms}, {"hold", hold_ms}, {"release", release_ms}});
    }
    std::unique_ptr<AudioEffect> make_compressor(f32 threshold_db, f32 ratio, f32 attack_ms, f32 release_ms, f32 makeup_db) {
        return with(create<CompressorEffect>(), {{"threshold", threshold_db}, {"ratio", ratio}, {"attack", attack_ms}, {"release", release_ms}, {"makeup", makeup_db}});
    }
    std::unique_ptr<AudioEffect> make_limiter(f32 ceiling_db, f32 release_ms, f32 lookahead_ms) {
        return with(create<LimiterEffect>(), {{"ceiling", ceiling_db}, {"release", release_ms}, {"lookahead", lookahead_ms}});
    }
    std::unique_ptr<AudioEffect> make_hum_filter(f32 base_hz, u32 harmonics) {
        return with(create<HumFilterEffect>(), {{"base_frequency", base_hz}, {"harmonics", static_cast<f32>(harmonics)}});
    }
    std::unique_ptr<AudioEffect> make_convolution(std::shared_ptr<const SampleBuffer> impulse, f32 wet, f32 dry, f32 gain_db, f32 predelay_ms) {
        return std::make_unique<ConvolutionEffect>(std::move(impulse), wet, dry, gain_db, predelay_ms);
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> apply_effects(const SampleBuffer &buffer, std::span<const std::unique_ptr<AudioEffect>> effects,
                                                                            bool include_tail, u32 block_frames) {
        if (!buffer.samples || buffer.channels == 0 || buffer.sample_rate == 0) {
            return std::unexpected("audio: nothing to process");
        }
        block_frames = std::max(block_frames, 16u);
        const u32 channels = buffer.channels;
        const u64 frames = buffer.frames();
        u64 latency = 0, tail = 0;
        for (const auto &effect : effects) {
            effect->prepare(static_cast<f32>(buffer.sample_rate), channels, block_frames);
            latency += effect->latency_frames();
            tail += effect->tail_frames();
        }
        const u64 total = frames + latency + (include_tail ? tail : 0);
        auto out = std::make_shared<std::vector<f32>>();
        out->reserve(static_cast<usize>(total) * channels);
        AudioBuffer block(channels, block_frames);
        for (u64 position = 0; position < total; position += block_frames) {
            const u32 count = static_cast<u32>(std::min<u64>(block_frames, total - position));
            block.clear();
            if (position < frames) {
                const usize available = static_cast<usize>(std::min<u64>(count, frames - position));
                block.load_interleaved(std::span<const f32>{*buffer.samples}.subspan(static_cast<usize>(position) * channels, available * channels), channels);
            }
            for (const auto &effect : effects) {
                effect->process(block);
            }
            const usize base = out->size();
            out->resize(base + static_cast<usize>(count) * channels);
            block.store_interleaved(std::span<f32>{*out}.subspan(base), channels, count);
        }
        // Remove the look-ahead so the result lines up with the input.
        const usize drop = static_cast<usize>(latency) * channels;
        if (drop > 0 && drop <= out->size()) {
            out->erase(out->begin(), out->begin() + static_cast<std::ptrdiff_t>(drop));
        }
        auto result = std::make_shared<SampleBuffer>();
        result->channels = channels;
        result->sample_rate = buffer.sample_rate;
        result->samples = std::move(out);
        result->markers = buffer.markers;
        result->loop = buffer.loop;
        return result;
    }

} // namespace SFT::Audio
