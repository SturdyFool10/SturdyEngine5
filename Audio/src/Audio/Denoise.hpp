#pragma once

#include <Audio/EffectBase.hpp>

#include <atomic>
#include <memory>
#include <vector>

/// Spectral noise reduction: a short-time Fourier transform attenuates every frequency bin by how far it stands above the noise
/// floor there (a Wiener-style gain with decision-directed smoothing, so the residue does not warble). The noise floor comes from
/// one of two places:
///  - Auto: tracked continuously per bin (it falls to quiet moments quickly and creeps up slowly), good for steady hiss, fans
///    and hum under changing material; nothing to configure.
///  - Profile: measured from a stretch of audio that holds only the noise (`begin_learning`/`end_learning`, or
///    `learn_noise_profile` on a recording), good for noise that never pauses or a voice that never stops.
namespace SFT::Audio {

    /// The average noise power per FFT bin, per channel.
    struct NoiseProfile {
        u32 fft_size = 0;
        u32 sample_rate = 0;
        std::vector<std::vector<f32>> power; ///< [channel][bin], `fft_size / 2 + 1` bins
        [[nodiscard]] bool valid() const noexcept { return fft_size != 0 && !power.empty(); }
    };

    /// The noise-reduction effect (also reachable as `EffectKind::NoiseReduction`). Latency is `fft_size - 1` frames.
    class NoiseReducer final : public detail::ParametricEffect {
      public:
        static constexpr u32 fft_size = 2048;
        NoiseReducer();
        ~NoiseReducer() override;

        [[nodiscard]] ustr name() const override { return "Noise reduction"_ustr; }
        void prepare(f32 sample_rate, u32 channels, u32 max_frames) override;
        void reset() override;
        void process(AudioBuffer &buffer) override;
        [[nodiscard]] u32 latency_frames() const override { return fft_size - 1; }
        [[nodiscard]] u32 tail_frames() const override { return fft_size; }

        /// Starts measuring the noise: audio passes through untouched while it listens. Game thread; allocates.
        void begin_learning();
        /// Stops listening and returns the profile (also installed, and `mode` set to Profile). Empty when nothing was heard.
        std::shared_ptr<const NoiseProfile> end_learning();
        /// Installs a profile measured earlier (any channel count: channel c uses profile channel c % count).
        void set_profile(std::shared_ptr<const NoiseProfile> profile);
        [[nodiscard]] std::shared_ptr<const NoiseProfile> profile() const;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };

    [[nodiscard]] std::unique_ptr<NoiseReducer> make_noise_reduction(f32 reduction_db = 12.0f);
    [[nodiscard]] std::span<const ParameterInfo> noise_reduction_parameters();

    /// The noise profile of `buffer` between two frames (the whole buffer by default), for use with `reduce_noise`.
    [[nodiscard]] std::expected<std::shared_ptr<const NoiseProfile>, UString> learn_noise_profile(const SampleBuffer &buffer, u64 start = 0, u64 end = ~0ull);

    /// Offline noise reduction of a whole buffer. With a profile the noise floor is that profile; without, it is tracked.
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> reduce_noise(const SampleBuffer &buffer, std::shared_ptr<const NoiseProfile> profile = nullptr,
                                                                                         f32 reduction_db = 12.0f);

} // namespace SFT::Audio
