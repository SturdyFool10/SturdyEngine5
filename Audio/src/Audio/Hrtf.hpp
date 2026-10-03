#pragma once

#include <Audio/Spatial.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

/// Measured head-related transfer functions for headphone rendering. A `HrtfSet` holds impulse-response pairs measured (or
/// modelled) on a grid of directions around a head, loaded from a SOFA file (`SimpleFreeFieldHRIR`, the format of the MIT KEMAR,
/// CIPIC, ARI, LISTEN and most other public databases). Plug it into a Binaural output with `make_hrtf_filter_factory`:
///
///     OutputDesc out{.kind = OutputDesc::Kind::Binaural, .name = "headphones"};
///     out.binaural = make_hrtf_filter_factory(*HrtfSet::from_sofa_file("kemar.sofa", engine_rate));
///
/// Each ear's response is split into an onset delay and a delay-free impulse response, so blending neighbouring directions does
/// not smear the interaural time difference: the responses are interpolated, the delays are interpolated separately and applied
/// as a fractional delay.
namespace SFT::Audio {

    class HrtfSet {
      public:
        /// Nearest measured directions (up to three) with the weights that blend them.
        struct Blend {
            std::array<u32, 3> index{};
            std::array<f32, 3> weight{};
            u32 count = 0;
        };

        /// Loads a SOFA file's impulse responses, converted to `sample_rate` and cut to `max_taps` taps (256 is plenty for
        /// localisation; each tap costs one multiply-add per sample per binaural voice).
        [[nodiscard]] static std::expected<std::shared_ptr<const HrtfSet>, UString> from_sofa(std::span<const std::byte> file, u32 sample_rate, u32 max_taps = 256);
        [[nodiscard]] static std::expected<std::shared_ptr<const HrtfSet>, UString> from_sofa_file(const std::filesystem::path &path, u32 sample_rate, u32 max_taps = 256);
        /// Builds a set from raw responses: `directions[m]` is the unit direction (listener space: +X right, +Y up, -Z forward) of
        /// measurement m, `left[m]`/`right[m]` its impulse responses (equal lengths, onset delay still in them).
        [[nodiscard]] static std::expected<std::shared_ptr<const HrtfSet>, UString> from_measurements(u32 sample_rate, std::span<const glm::vec3> directions,
                                                                                                        std::span<const std::vector<f32>> left,
                                                                                                        std::span<const std::vector<f32>> right, u32 max_taps = 256);

        [[nodiscard]] u32 sample_rate() const noexcept { return sample_rate_; }
        [[nodiscard]] u32 taps() const noexcept { return taps_; }
        [[nodiscard]] u32 measurement_count() const noexcept { return static_cast<u32>(directions_.size()); }
        [[nodiscard]] const glm::vec3 &direction(u32 measurement) const noexcept { return directions_[measurement]; }
        /// The delay-free response of one ear (`ear` 0 = left, 1 = right) and the onset delay it was split from, in samples.
        [[nodiscard]] std::span<const f32> response(u32 measurement, u32 ear) const noexcept { return {irs_.data() + (static_cast<usize>(measurement) * 2 + ear) * taps_, taps_}; }
        [[nodiscard]] f32 onset(u32 measurement, u32 ear) const noexcept { return onsets_[static_cast<usize>(measurement) * 2 + ear]; }
        [[nodiscard]] Blend nearest(const glm::vec3 &direction) const noexcept;

      private:
        HrtfSet() = default;
        u32 sample_rate_ = 48000;
        u32 taps_ = 0;
        std::vector<glm::vec3> directions_;
        std::vector<f32> irs_;    // [measurement][ear][tap]
        std::vector<f32> onsets_; // [measurement][ear]
    };

    /// A binaural filter that renders through `set` (shared by every voice's filter).
    [[nodiscard]] std::unique_ptr<BinauralFilter> make_hrtf_filter(std::shared_ptr<const HrtfSet> set);
    [[nodiscard]] BinauralFilterFactory make_hrtf_filter_factory(std::shared_ptr<const HrtfSet> set);

} // namespace SFT::Audio
