#include <Audio/Denoise.hpp>

#include <Audio/Fft.hpp>
#include <Audio/Interleaved.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>

namespace SFT::Audio {

    namespace {
        constexpr u32 kN = NoiseReducer::fft_size;
        constexpr u32 kHop = kN / 4;
        constexpr u32 kBins = kN / 2 + 1;

        const UString kModeNames[] = {"Auto", "Profile"};
        const ParameterInfo kParams[] = {
            {"reduction", "dB", 0, 60, 12},      {"sensitivity", "dB", -12, 12, 0},         {"smoothing", "", 0, 0.95f, 0.5f},
            {"adapt_rate", "dB/s", 0, 30, 3},    {"mode", "", 0, 1, 0, false, kModeNames},
        };
        enum Param : u32 { Reduction, Sensitivity, Smoothing, AdaptRate, NoiseMode };

        /// Square-root Hann: analysis and synthesis windows whose product sums to a constant at 75% overlap.
        const std::vector<f32> &window() {
            static const std::vector<f32> w = [] {
                std::vector<f32> out(kN);
                for (u32 i = 0; i < kN; ++i) {
                    out[i] = std::sqrt(0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / static_cast<f32>(kN)));
                }
                return out;
            }();
            return w;
        }

        const Fft &fft() {
            static const Fft instance(kN);
            return instance;
        }

        /// Everything one channel carries from block to block.
        struct ChannelState {
            std::vector<f32> in_ring = std::vector<f32>(kN, 0.0f);   // last kN input samples, indexed by absolute time mod kN
            std::vector<f32> out_ring = std::vector<f32>(kN, 0.0f);  // overlap-add accumulator, same indexing
            std::vector<f32> noise = std::vector<f32>(kBins, 0.0f);  // tracked noise power
            std::vector<f32> smooth = std::vector<f32>(kBins, 0.0f); // time-averaged power the tracker follows
            std::vector<f32> clean = std::vector<f32>(kBins, 0.0f);  // previous frame's estimated clean power
            std::vector<f32> gain = std::vector<f32>(kBins, 1.0f);   // previous frame's gain
            std::vector<f32> raw_gain = std::vector<f32>(kBins, 1.0f);
            std::vector<f32> frame = std::vector<f32>(kN), re = std::vector<f32>(kBins), im = std::vector<f32>(kBins), scratch = std::vector<f32>(kN);
            u64 count = 0;
            bool noise_seeded = false;
        };

        struct Settings {
            f32 floor_gain, sensitivity, smoothing, rise_per_frame;
            bool use_profile;
        };
    } // namespace

    struct NoiseReducer::State {
        std::vector<ChannelState> channels;
        std::atomic<std::shared_ptr<const NoiseProfile>> profile;
        // Learning: sums of bin power per channel, filled by the audio thread while `learning` is set.
        struct Accumulator {
            std::vector<std::vector<f64>> sum;
            u64 frames = 0;
        };
        std::mutex learn_mutex;
        std::shared_ptr<Accumulator> accumulator;
        std::atomic<bool> learning{false};
        f32 sample_rate = 48000.0f;

        /// Runs one spectral frame for channel `c` ending at the current time.
        void run_frame(u32 c, ChannelState &ch, const Settings &s, const NoiseProfile *profile, Accumulator *learn) {
            const std::vector<f32> &w = window();
            for (u32 j = 0; j < kN; ++j) {
                ch.frame[j] = ch.in_ring[(ch.count + j) % kN] * w[j];
            }
            fft().forward_real(ch.frame.data(), ch.re.data(), ch.im.data(), ch.scratch.data());
            if (learn != nullptr) {
                auto &sum = learn->sum[c];
                for (u32 k = 0; k < kBins; ++k) {
                    sum[k] += static_cast<f64>(ch.re[k]) * ch.re[k] + static_cast<f64>(ch.im[k]) * ch.im[k];
                }
                if (c == 0) {
                    ++learn->frames;
                }
            } else {
                const std::vector<f32> *fixed = s.use_profile && profile != nullptr ? &profile->power[c % profile->power.size()] : nullptr;
                for (u32 k = 0; k < kBins; ++k) {
                    const f32 power = ch.re[k] * ch.re[k] + ch.im[k] * ch.im[k];
                    if (fixed != nullptr) {
                        ch.noise[k] = (*fixed)[k];
                    } else if (!ch.noise_seeded) {
                        ch.smooth[k] = ch.noise[k] = power;
                    } else if (ch.smooth[k] = 0.9f * ch.smooth[k] + 0.1f * power; ch.smooth[k] < ch.noise[k]) {
                        ch.noise[k] = 0.7f * ch.noise[k] + 0.3f * ch.smooth[k];
                    } else {
                        ch.noise[k] *= s.rise_per_frame;
                    }
                    const f32 noise = std::max(ch.noise[k] * s.sensitivity, 1e-12f);
                    const f32 posterior = power / noise;
                    const f32 prior = 0.98f * ch.clean[k] / noise + 0.02f * std::max(posterior - 1.0f, 0.0f);
                    ch.raw_gain[k] = std::max(prior / (1.0f + prior), s.floor_gain);
                }
                ch.noise_seeded = true;
                for (u32 k = 0; k < kBins; ++k) {
                    // Smooth across three bins to break up isolated bins that would sound like chirps, then across time on the way down.
                    const f32 neighbours = 0.25f * ch.raw_gain[k == 0 ? 0 : k - 1] + 0.5f * ch.raw_gain[k] + 0.25f * ch.raw_gain[std::min(k + 1, kBins - 1)];
                    const f32 target = std::max(neighbours, s.floor_gain);
                    ch.gain[k] = target > ch.gain[k] ? target : s.smoothing * ch.gain[k] + (1.0f - s.smoothing) * target;
                    const f32 power = ch.re[k] * ch.re[k] + ch.im[k] * ch.im[k];
                    ch.clean[k] = ch.gain[k] * ch.gain[k] * power;
                    ch.re[k] *= ch.gain[k];
                    ch.im[k] *= ch.gain[k];
                }
            }
            fft().inverse_real(ch.re.data(), ch.im.data(), ch.frame.data(), ch.scratch.data());
            // The squared window sums to 2 at this overlap.
            for (u32 j = 0; j < kN; ++j) {
                ch.out_ring[(ch.count + j) % kN] += 0.5f * ch.frame[j] * w[j];
            }
        }
    };

    NoiseReducer::NoiseReducer() : ParametricEffect(kParams), state_(std::make_unique<State>()) {}
    NoiseReducer::~NoiseReducer() = default;

    void NoiseReducer::prepare(f32 sample_rate, u32 channels, u32 max_frames) {
        ParametricEffect::prepare(sample_rate, channels, max_frames);
        state_->sample_rate = sample_rate;
        state_->channels.assign(channels, ChannelState{});
    }

    void NoiseReducer::reset() {
        const u32 channels = static_cast<u32>(state_->channels.size());
        state_->channels.assign(channels, ChannelState{});
    }

    void NoiseReducer::begin_learning() {
        auto acc = std::make_shared<State::Accumulator>();
        acc->sum.assign(state_->channels.size(), std::vector<f64>(kBins, 0.0));
        {
            std::lock_guard lock(state_->learn_mutex);
            state_->accumulator = std::move(acc);
        }
        state_->learning.store(true, std::memory_order_release);
    }

    std::shared_ptr<const NoiseProfile> NoiseReducer::end_learning() {
        state_->learning.store(false, std::memory_order_release);
        std::shared_ptr<State::Accumulator> acc;
        {
            std::lock_guard lock(state_->learn_mutex);
            acc = std::move(state_->accumulator);
        }
        if (!acc || acc->frames == 0) {
            return nullptr;
        }
        auto profile = std::make_shared<NoiseProfile>();
        profile->fft_size = kN;
        profile->sample_rate = static_cast<u32>(state_->sample_rate);
        for (const auto &channel : acc->sum) {
            std::vector<f32> power(kBins);
            std::ranges::transform(channel, power.begin(), [&](f64 total) { return static_cast<f32>(total / static_cast<f64>(acc->frames)); });
            profile->power.push_back(std::move(power));
        }
        set_profile(profile);
        set_parameter(NoiseMode, 1.0f);
        return profile;
    }

    void NoiseReducer::set_profile(std::shared_ptr<const NoiseProfile> profile) {
        if (profile && (profile->fft_size != kN || profile->power.empty())) {
            return;
        }
        state_->profile.store(std::move(profile));
    }

    std::shared_ptr<const NoiseProfile> NoiseReducer::profile() const { return state_->profile.load(); }

    void NoiseReducer::process(AudioBuffer &buffer) {
        (void)take_dirty();
        Settings s;
        s.floor_gain = std::pow(10.0f, -value(Reduction) / 20.0f);
        s.sensitivity = std::pow(10.0f, value(Sensitivity) / 10.0f) * 2.0f; // the minimum-tracking floor sits below the mean noise power
        s.smoothing = value(Smoothing);
        s.rise_per_frame = std::pow(10.0f, value(AdaptRate) / 10.0f * static_cast<f32>(kHop) / sample_rate_);
        s.use_profile = value(NoiseMode) >= 0.5f;
        const std::shared_ptr<const NoiseProfile> profile = s.use_profile ? state_->profile.load() : nullptr;
        if (s.use_profile && !profile) {
            s.use_profile = false; // no profile yet: track instead of passing everything
        }
        std::shared_ptr<State::Accumulator> learn;
        if (state_->learning.load(std::memory_order_acquire)) {
            std::lock_guard lock(state_->learn_mutex);
            learn = state_->accumulator;
        }
        const u32 channels = std::min<u32>(active_channels(buffer), static_cast<u32>(state_->channels.size()));
        for (const u32 c : std::views::iota(u32{0}, channels)) {
            ChannelState &ch = state_->channels[c];
            for (f32 &sample : buffer.channel(c)) {
                ch.in_ring[ch.count % kN] = sample;
                ++ch.count;
                if (ch.count >= kN && ch.count % kHop == 0) {
                    // The frame covers the last kN samples; `count` indexes its first one modulo kN.
                    state_->run_frame(c, ch, s, profile.get(), learn.get());
                }
                // The sample leaving now is the one kN-1 steps back: complete once the frame ending here has been added.
                f32 &slot = ch.out_ring[ch.count % kN];
                sample = learn ? ch.in_ring[ch.count % kN] : slot;
                slot = 0.0f;
            }
        }
    }

    std::unique_ptr<NoiseReducer> make_noise_reduction(f32 reduction_db) {
        auto effect = std::make_unique<NoiseReducer>();
        effect->set_parameter(Reduction, reduction_db);
        return effect;
    }

    std::span<const ParameterInfo> noise_reduction_parameters() { return kParams; }

    namespace {
        /// Runs a prepared effect over a whole buffer; the shared body of the offline entry points.
        std::expected<std::shared_ptr<SampleBuffer>, UString> run_offline(const SampleBuffer &buffer, std::unique_ptr<AudioEffect> effect) {
            std::unique_ptr<AudioEffect> chain[] = {std::move(effect)};
            return apply_effects(buffer, chain, /*include_tail=*/false);
        }
    } // namespace

    std::expected<std::shared_ptr<const NoiseProfile>, UString> learn_noise_profile(const SampleBuffer &buffer, u64 start, u64 end) {
        end = std::min(end, buffer.frames());
        if (!buffer.samples || buffer.channels == 0 || start >= end || end - start < kN) {
            return std::unexpected("audio: a noise profile needs at least one FFT frame (2048 samples) of noise");
        }
        auto reducer = std::make_unique<NoiseReducer>();
        reducer->prepare(static_cast<f32>(buffer.sample_rate), buffer.channels, 4096);
        reducer->begin_learning();
        const usize c = buffer.channels;
        const std::span<const f32> region = std::span<const f32>{*buffer.samples}.subspan(static_cast<usize>(start) * c, static_cast<usize>(end - start) * c);
        AudioBuffer block(buffer.channels, 4096);
        for (usize offset = 0; offset < region.size(); offset += 4096 * c) {
            const usize frames = std::min<usize>(4096, (region.size() - offset) / c);
            block.clear();
            block.load_interleaved(region.subspan(offset, frames * c), buffer.channels);
            reducer->process(block);
        }
        auto profile = reducer->end_learning();
        if (!profile) {
            return std::unexpected("audio: no noise was heard");
        }
        return profile;
    }

    std::expected<std::shared_ptr<SampleBuffer>, UString> reduce_noise(const SampleBuffer &buffer, std::shared_ptr<const NoiseProfile> profile, f32 reduction_db) {
        auto reducer = make_noise_reduction(reduction_db);
        if (profile) {
            if (profile->fft_size != kN) {
                return std::unexpected("audio: the noise profile was made with a different FFT size");
            }
            reducer->set_profile(std::move(profile));
            reducer->set_parameter(NoiseMode, 1.0f);
        }
        return run_offline(buffer, std::move(reducer));
    }

} // namespace SFT::Audio
