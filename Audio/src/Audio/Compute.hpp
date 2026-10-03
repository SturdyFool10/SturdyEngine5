#pragma once

#include <Foundation/Foundation.hpp>

#include <memory>
#include <span>
#include <vector>

/// Compute-mode mixing: the part of a block that is pure arithmetic (resample a loaded sample, low-pass it, scale it by a gain ramp, add
/// it into an output channel) is described as flat records and handed to a `ComputeMixBackend`, which may run it on a GPU. The
/// mixer plans a block for every voice simple enough to qualify, submits it at the start of the block, mixes everything else on
/// the CPU, and adds the backend's result to the buses at the end. A backend is therefore an accelerator for volume: tens of
/// thousands of one-shot samples cost a few dispatches instead of a few thousand CPU kernels.
///
/// The arithmetic is fixed so every backend agrees (see `reference_mix`): position of frame `i` of a voice is
/// `base + frac + (i - first_frame) * step`, evaluated in f32 from the integer base so long samples do not lose precision;
/// the sample is read with Catmull-Rom interpolation, wrapping when `loop` is set and reading zero outside the sample
/// otherwise; a tap's gain ramps linearly from `gain_from` to `gain_to` across the block like `Kernels::add_ramp`.
namespace SFT::Audio {

    /// A sample resident in the backend. 0 is never a valid id.
    using ComputeSampleId = u32;

    /// One voice's first stage: read the sample (resampled), optionally average its channels to one, optionally low-pass,
    /// producing `signal_count` planes of `frames` floats. Filtering runs over the whole block (the input is zero outside the
    /// frames that carry audio), exactly like the CPU path's per-voice low-pass.
    struct ComputeVoice {
        ComputeSampleId sample = 0;
        u32 loop = 0;
        /// Frames of the block before the voice starts sounding, and how many carry audio after that.
        u32 first_frame = 0;
        u32 valid_frames = 0;
        /// Integer part and fraction of the source position of frame `first_frame`, and the source frames advanced per frame.
        i64 base = 0;
        f32 frac = 0.0f;
        f32 step = 1.0f;
        /// Index of this voice's first plane among the block's signals. With `mix_down` there is one plane (the average of the
        /// sample's channels), otherwise one per channel.
        u32 first_signal = 0;
        u32 mix_down = 0;
        /// Direct-form-II-transposed low-pass (the mixer's `Biquad`); only with `mix_down`. State goes in and comes back out.
        u32 filter = 0;
        f32 b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        f32 z1 = 0.0f, z2 = 0.0f;
    };

    /// Second stage: one signal plane scaled by a gain ramp and added into one destination channel.
    struct ComputeTap {
        u32 signal = 0;
        u32 destination = 0; ///< index into the result (`destination_count` planes of `frames`)
        f32 gain_from = 0.0f;
        f32 gain_to = 0.0f;
    };

    struct ComputeBlock {
        u32 frames = 0;
        u32 destination_count = 0;
        u32 signal_count = 0;
        std::span<const ComputeVoice> voices;
        /// Grouped by destination (ascending) so a backend can reduce each destination without atomics.
        std::span<const ComputeTap> taps;
    };

    struct ComputeResult {
        /// `destination_count * frames` floats, destination-major.
        std::span<const f32> mix;
        /// Two floats (z1, z2) per voice: the filter state after the block.
        std::span<const f32> filter_state;
    };

    struct ComputeBackendInfo {
        UString name;
        /// Largest voice and tap counts per block the backend takes before the mixer keeps the rest on the CPU.
        u32 max_voices = 1u << 16;
        u32 max_taps = 1u << 18;
    };

    class ComputeMixBackend {
      public:
        virtual ~ComputeMixBackend() = default;
        [[nodiscard]] virtual ComputeBackendInfo info() const = 0;

        /// Makes interleaved samples resident. May be called from any thread and must not block on the device: a sample
        /// that is not yet uploaded reports `sample_ready() == false` and the mixer plays it on the CPU meanwhile.
        [[nodiscard]] virtual ComputeSampleId register_sample(std::shared_ptr<const std::vector<f32>> samples, u32 channels) = 0;
        [[nodiscard]] virtual bool sample_ready(ComputeSampleId id) const = 0;
        virtual void release_sample(ComputeSampleId id) = 0;

        /// Starts mixing `block`; the spans stay valid until `collect` returns.
        virtual void submit(const ComputeBlock &block) = 0;
        /// Waits for the submitted block. The spans are valid until the next `submit`.
        [[nodiscard]] virtual ComputeResult collect() = 0;
    };

    /// The reference arithmetic for a voice's first stage, used by the CPU backend and by tests to check a device against.
    /// Writes `signal_count` planes of `frames` floats to `planes` (overwriting) and returns the filter state in `state`.
    void reference_voice_signals(const ComputeVoice &voice, const f32 *samples, u32 channels, u64 sample_frames, u32 frames, f32 *planes, f32 *state) noexcept;
    /// Second stage for one tap.
    void reference_tap(const ComputeTap &tap, const f32 *signal, u32 frames, f32 *destination) noexcept;

    /// A backend that runs the arithmetic on the CPU, serially. Always available; it is what the GPU path is checked
    /// against, and a sensible choice on machines without a usable device.
    [[nodiscard]] std::unique_ptr<ComputeMixBackend> make_cpu_compute_backend();

} // namespace SFT::Audio
