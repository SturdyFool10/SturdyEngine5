#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// Editing decoded audio. Every function takes a `SampleBuffer` and returns a new one (buffers are immutable and shared, so
/// the original is untouched and an editor gets undo for free by keeping the old pointers). Positions are in frames; use
/// `frames_at(buffer, seconds)` to convert. Markers and loop regions follow the edits: cut a section out and the markers after
/// it move up, markers inside it go away.
///
/// For files too large to hold in memory, use `transcode` (Encode.hpp), which streams through the same codecs, or run effects
/// over a stream; these functions are for audio that fits in RAM (sound effects, voice lines, music stems up to hours long).
namespace SFT::Audio::edit {

    using Buffer = std::shared_ptr<SampleBuffer>;
    using Result = std::expected<Buffer, UString>;

    struct Range {
        u64 start = 0;
        u64 end = 0; // exclusive
        [[nodiscard]] u64 length() const noexcept { return end > start ? end - start : 0; }
    };

    enum class FadeCurve : u8 {
        Linear,
        EqualPower, ///< sine/cosine: constant power across a crossfade of uncorrelated material
        Exponential,///< slow start, fast finish (a natural-sounding fade-in)
        Logarithmic,///< fast start, slow finish (a natural-sounding fade-out)
        SCurve,     ///< smoothstep
    };

    [[nodiscard]] u64 frames_at(const SampleBuffer &buffer, f64 seconds) noexcept;
    [[nodiscard]] f64 seconds_at(const SampleBuffer &buffer, u64 frames) noexcept;

    // ---- structure ------------------------------------------------------------------------------------------------------------

    /// Frames [start, end) as a new buffer.
    [[nodiscard]] Buffer slice(const SampleBuffer &buffer, u64 start, u64 end);
    /// Everything except [start, end).
    [[nodiscard]] Buffer remove(const SampleBuffer &buffer, u64 start, u64 end);
    /// `piece` spliced in at frame `at`. A piece with other channels or rate is converted to match first.
    [[nodiscard]] Buffer insert(const SampleBuffer &buffer, u64 at, const SampleBuffer &piece);
    /// Buffers back to back (converted to the first one's format), optionally overlapping by `crossfade_frames`.
    [[nodiscard]] Result concat(std::span<const SampleBuffer *const> buffers, u64 crossfade_frames = 0, FadeCurve curve = FadeCurve::EqualPower);
    [[nodiscard]] Buffer repeat(const SampleBuffer &buffer, u32 times);
    [[nodiscard]] Buffer reverse(const SampleBuffer &buffer);
    /// Silence of `frames` length in the format of `like`.
    [[nodiscard]] Buffer silence(const SampleBuffer &like, u64 frames);

    // ---- level and fades ---------------------------------------------------------------------------------------------------------

    [[nodiscard]] Buffer gain(const SampleBuffer &buffer, f32 decibels);
    /// Gain slid from `from_gain` to `to_gain` (linear) over [start, end) along `curve`; outside the range the gain holds.
    [[nodiscard]] Buffer fade(const SampleBuffer &buffer, u64 start, u64 end, f32 from_gain, f32 to_gain, FadeCurve curve = FadeCurve::Linear);
    [[nodiscard]] Buffer fade_in(const SampleBuffer &buffer, u64 frames, FadeCurve curve = FadeCurve::Exponential);
    [[nodiscard]] Buffer fade_out(const SampleBuffer &buffer, u64 frames, FadeCurve curve = FadeCurve::Logarithmic);
    /// Piecewise-linear gain automation: (frame, linear gain) points in order; held flat before the first and after the last.
    [[nodiscard]] Buffer gain_envelope(const SampleBuffer &buffer, std::span<const std::pair<u64, f32>> points);
    /// `a` with `b` mixed in starting at frame `offset` (the result is as long as the longer of the two).
    [[nodiscard]] Buffer mix(const SampleBuffer &a, const SampleBuffer &b, u64 offset = 0, f32 gain_a = 1.0f, f32 gain_b = 1.0f);
    [[nodiscard]] Buffer remove_dc(const SampleBuffer &buffer);

    struct NormalizeOptions {
        /// Peak target in dBFS, used when `target_lufs` is unset.
        f32 target_peak_db = -1.0f;
        /// Integrated loudness target (EBU R128 programme loudness, e.g. -23 broadcast, -16 podcasts, -14 streaming).
        std::optional<f32> target_lufs;
        /// In LUFS mode the gain is reduced if it would push the sample peak past this (a safeguard, not a limiter).
        f32 ceiling_db = -1.0f;
    };
    [[nodiscard]] Result normalize(const SampleBuffer &buffer, const NormalizeOptions &options = {});

    // ---- silence ------------------------------------------------------------------------------------------------------------------

    /// Stretches where every channel stays below `threshold_db` for at least `min_ms`.
    [[nodiscard]] std::vector<Range> find_silence(const SampleBuffer &buffer, f32 threshold_db = -60.0f, f32 min_ms = 100.0f);
    /// Cuts silence from the start and end, leaving `keep_ms` of it.
    [[nodiscard]] Buffer trim_silence(const SampleBuffer &buffer, f32 threshold_db = -60.0f, f32 keep_ms = 10.0f);

    // ---- channels, rate ----------------------------------------------------------------------------------------------------------------

    /// Folds or spreads the channels onto `channels` by role (5.1 to stereo, mono to stereo...).
    [[nodiscard]] Buffer convert_channels(const SampleBuffer &buffer, u32 channels);
    [[nodiscard]] Buffer extract_channels(const SampleBuffer &buffer, std::span<const u32> channels);
    /// Mono tracks (or any buffers) side by side as one multi-channel buffer (they must share rate and length; the shortest wins).
    [[nodiscard]] Result combine_channels(std::span<const SampleBuffer *const> tracks);
    /// Left/right to mid/side and back (stereo only; other buffers are returned unchanged).
    [[nodiscard]] Buffer mid_side_encode(const SampleBuffer &buffer);
    [[nodiscard]] Buffer mid_side_decode(const SampleBuffer &buffer);
    /// Balance of a stereo buffer, -1 (left) to 1 (right).
    [[nodiscard]] Buffer pan(const SampleBuffer &buffer, f32 balance);
    [[nodiscard]] Buffer convert_rate(const SampleBuffer &buffer, u32 sample_rate);

    // ---- time and pitch --------------------------------------------------------------------------------------------------------------------

    /// Plays `factor` times faster with the pitch following (tape speed): 2.0 is an octave up, half the length.
    [[nodiscard]] Buffer change_speed(const SampleBuffer &buffer, f64 factor);
    /// Changes duration by `ratio` (2.0 = twice as long) keeping the pitch (waveform-similarity overlap-add).
    [[nodiscard]] Buffer time_stretch(const SampleBuffer &buffer, f64 ratio);
    /// Changes the pitch by `semitones` keeping the duration.
    [[nodiscard]] Buffer pitch_shift(const SampleBuffer &buffer, f64 semitones);

    // ---- bit depth ---------------------------------------------------------------------------------------------------------------------------

    /// Reduces to `bits` of resolution with optional triangular dither (to prepare 16-bit masters or for a lo-fi effect).
    [[nodiscard]] Buffer quantize(const SampleBuffer &buffer, u32 bits, bool dither = true);

} // namespace SFT::Audio::edit
