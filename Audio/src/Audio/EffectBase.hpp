#pragma once

#include <Audio/Effects.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace SFT::Audio::detail {

    /// An effect whose controls are a table of named values. Subclasses read `value(i)` and rebuild whatever depends on
    /// the values when `take_dirty()` says one changed (checked once per block, never per sample).
    class ParametricEffect : public AudioEffect {
      public:
        [[nodiscard]] std::span<const ParameterInfo> parameters() const override { return infos_; }
        void set_parameter(u32 index, f32 value) override {
            if (index < values_.size()) {
                values_[index] = std::clamp(value, infos_[index].minimum, infos_[index].maximum);
                dirty_ = true;
            }
        }
        [[nodiscard]] f32 parameter(u32 index) const override { return index < values_.size() ? values_[index] : 0.0f; }
        using AudioEffect::set_parameter;

        void prepare(f32 sample_rate, u32 channels, u32 max_frames) override {
            sample_rate_ = sample_rate;
            channels_ = channels;
            max_frames_ = max_frames;
            on_prepare();
            dirty_ = true;
        }

      protected:
        explicit ParametricEffect(std::span<const ParameterInfo> infos) : infos_(infos), values_(infos.size()) {
            for (usize i = 0; i < infos.size(); ++i) {
                values_[i] = infos[i].default_value;
            }
        }
        virtual void on_prepare() {}

        [[nodiscard]] f32 value(u32 index) const noexcept { return values_[index]; }
        [[nodiscard]] bool flag(u32 index) const noexcept { return values_[index] >= 0.5f; }
        bool take_dirty() noexcept {
            const bool d = dirty_;
            dirty_ = false;
            return d;
        }
        /// Channels both prepared for and present in `buffer`.
        [[nodiscard]] u32 active_channels(const AudioBuffer &buffer) const noexcept { return std::min(channels_, buffer.channels()); }
        [[nodiscard]] f32 ms_to_samples(f32 ms) const noexcept { return ms * 0.001f * sample_rate_; }
        /// One-pole smoothing coefficient: the fraction of the gap closed per sample for a time constant of `ms`.
        [[nodiscard]] f32 time_coefficient(f32 ms) const noexcept {
            return ms <= 0.0f ? 0.0f : std::exp(-1.0f / (ms * 0.001f * sample_rate_));
        }

        std::span<const ParameterInfo> infos_;
        std::vector<f32> values_;
        bool dirty_ = true;
        f32 sample_rate_ = 48000.0f;
        u32 channels_ = 0;
        u32 max_frames_ = 0;
    };

} // namespace SFT::Audio::detail
