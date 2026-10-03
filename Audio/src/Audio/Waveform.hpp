#pragma once

#include <Audio/Source.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Everything needed to *show* audio: waveform peak data that stays cheap however long the audio is and however far you zoom
/// (`PeakPyramid`), a CPU renderer for waveforms, spectrograms, oscilloscopes and goniometers that produces plain RGBA images
/// (so they can be uploaded as textures or written out as files), and the raw column data for UIs that prefer to draw it
/// themselves.
namespace SFT::Audio {

    /// What one horizontal pixel of a waveform shows for one channel: the lowest and highest sample, and the RMS level.
    struct PeakColumn {
        f32 min = 0.0f;
        f32 max = 0.0f;
        f32 rms = 0.0f;
    };

    /// Min/max/RMS summaries of audio at several zoom levels (each level halves the resolution of the one below), built in one
    /// streaming pass. Asking for any view costs time proportional to the number of columns, not the length of the audio, so a
    /// ten-hour recording scrolls as smoothly as a gunshot. Can be filled incrementally while recording, and saved to disk next to
    /// the source file so a big file is only analysed once.
    class PeakPyramid {
      public:
        PeakPyramid() = default;
        PeakPyramid(u32 channels, u32 sample_rate, u32 base_frames_per_bucket = 256);

        // ---- building
        /// Adds interleaved frames (any block size, any number of times). The last partial bucket is summarised by `finish`.
        void append(const f32 *interleaved, usize frames);
        void finish();
        [[nodiscard]] static PeakPyramid from_buffer(const SampleBuffer &buffer, u32 base_frames_per_bucket = 256);
        /// Decodes `path` in blocks (constant memory) and summarises it. `progress` gets 0..1 and may return false to cancel.
        /// When `cache_file` is given and holds a pyramid for this exact file (size and modification time), it is loaded instead.
        [[nodiscard]] static std::expected<PeakPyramid, UString> from_file(const std::filesystem::path &path, u32 base_frames_per_bucket = 256,
                                                                               const std::function<bool(f32)> &progress = {},
                                                                               const std::optional<std::filesystem::path> &cache_file = std::nullopt);
        [[nodiscard]] std::expected<void, UString> save(const std::filesystem::path &path, const std::filesystem::path &source = {}) const;
        [[nodiscard]] static std::expected<PeakPyramid, UString> load(const std::filesystem::path &path, const std::filesystem::path &source = {});

        // ---- queries
        [[nodiscard]] u32 channels() const noexcept { return channels_; }
        [[nodiscard]] u32 sample_rate() const noexcept { return sample_rate_; }
        [[nodiscard]] u64 frames() const noexcept { return total_frames_; }
        [[nodiscard]] u32 base_frames_per_bucket() const noexcept { return base_; }
        /// `columns` equal slices of frames [start, end) for one channel (`channel == ~0u` merges all channels).
        [[nodiscard]] std::vector<PeakColumn> columns(u64 start, u64 end, u32 columns, u32 channel = ~0u) const;
        /// Summary of a whole range.
        [[nodiscard]] PeakColumn range(u64 start, u64 end, u32 channel = ~0u) const;

      private:
        struct Level {
            u64 frames_per_bucket = 0;
            std::vector<f32> min, max, mean_square; // bucket-major: [bucket * channels + channel]
            [[nodiscard]] usize buckets(u32 channels) const { return min.size() / channels; }
        };
        void push_bucket(u32 level, const f32 *mins, const f32 *maxs, const f32 *squares);
        /// Closes the bucket being filled (however many frames it has) and starts the next.
        void flush_partial();

        u32 channels_ = 0;
        u32 sample_rate_ = 48000;
        u32 base_ = 256;
        u64 total_frames_ = 0;
        std::vector<Level> levels_;
        // The bucket being filled.
        std::vector<f32> partial_min_, partial_max_, partial_square_;
        u32 partial_frames_ = 0;
        // A completed bucket at each level waiting for its sibling.
        std::vector<std::vector<f32>> pending_; // per level: [min*c, max*c, square*c] or empty
    };

    /// Reads `frames` frames starting at `start` straight from a file (for zoomed-in drawing beyond the pyramid's resolution).
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> read_frames(const std::filesystem::path &path, u64 start, u64 frames);

    // ---- images --------------------------------------------------------------------------------------------------------------------

    /// Tightly packed RGBA8, top row first, straight (non-premultiplied) alpha.
    struct Image {
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> rgba;
        [[nodiscard]] bool empty() const noexcept { return rgba.empty(); }
    };

    /// 0xRRGGBBAA
    using Color = u32;

    enum class WaveformMode : u8 {
        Envelope,     ///< filled between the min and max of each column
        EnvelopeRms,  ///< the envelope with a brighter core showing RMS level (what DAWs show)
        Line,         ///< connected samples when zoomed in far enough to see them (needs the samples); the envelope otherwise
        Bars,         ///< separated vertical bars (voice-message style)
    };

    enum class ChannelArrangement : u8 {
        Stacked,  ///< one lane per channel
        Overlay,  ///< all channels in one lane, drawn over each other
        Merged,   ///< all channels folded into one envelope
    };

    struct WaveformStyle {
        WaveformMode mode = WaveformMode::EnvelopeRms;
        ChannelArrangement channels = ChannelArrangement::Stacked;
        Color background = 0x11161BFF;
        Color wave = 0x4FA3D8FF;
        Color rms = 0xB8E3FFFF;
        Color center_line = 0x2C3A44FF;
        Color clipping = 0xFF4D4DFF;
        Color grid = 0x1D272EFF;
        Color marker = 0xFFC247FF;
        Color playhead = 0xFFFFFFFF;
        Color selection = 0x3D8BFF55;
        bool draw_center_line = true;
        bool draw_clipping = true;
        bool draw_grid = true;
        bool draw_markers = true;
        /// Show amplitude on a decibel scale (quiet detail becomes visible) instead of linear.
        bool decibel_scale = false;
        f32 decibel_floor = -60.0f;
        /// Multiplies amplitude before drawing (2 doubles the visual height; clipped to the lane).
        f32 vertical_zoom = 1.0f;
        u32 lane_gap = 2;
        /// Bar width and gap for `Bars` (pixels).
        u32 bar_width = 3;
        u32 bar_gap = 1;
        /// Optional highlighted frame range and playhead position (in frames).
        std::optional<std::pair<u64, u64>> selection_range;
        std::optional<u64> playhead_frame;
    };

    /// Draws frames [start, end) of `peaks` at `width` x `height`. `markers` are drawn as vertical lines; `exact` (the decoded
    /// samples, optional) lets `Line` mode and tight zooms draw true sample values instead of bucket summaries.
    [[nodiscard]] Image render_waveform(const PeakPyramid &peaks, u64 start, u64 end, u32 width, u32 height, const WaveformStyle &style = {},
                                        std::span<const AudioMarker> markers = {}, const SampleBuffer *exact = nullptr);
    /// The same straight from a buffer (builds the summaries for just this view).
    [[nodiscard]] Image render_waveform(const SampleBuffer &buffer, u64 start, u64 end, u32 width, u32 height, const WaveformStyle &style = {});

    // ---- spectrograms ---------------------------------------------------------------------------------------------------------------------

    enum class FrequencyScale : u8 { Linear, Log, Mel };
    enum class ColorMap : u8 { Gray, Viridis, Magma, Inferno, Heat, Ice };
    enum class WindowFunction : u8 { Hann, Hamming, BlackmanHarris };

    struct SpectrogramOptions {
        u32 fft_size = 2048;
        /// Frames between columns; 0 picks one hop per output column for the requested width.
        u32 hop = 0;
        /// Columns to produce when `hop` is 0.
        u32 columns = 1024;
        u32 rows = 512;
        WindowFunction window = WindowFunction::Hann;
        FrequencyScale scale = FrequencyScale::Log;
        f32 min_hz = 20.0f;
        f32 max_hz = 0.0f; ///< 0 = Nyquist
        /// Level range mapped to 0..1, in dB relative to a full-scale sine.
        f32 db_floor = -90.0f;
        f32 db_ceiling = 0.0f;
        /// Channel to analyse; ~0u mixes all channels to mono.
        u32 channel = ~0u;
    };

    struct Spectrogram {
        u32 columns = 0;
        u32 rows = 0;
        u64 start_frame = 0;
        u64 end_frame = 0;
        u32 sample_rate = 48000;
        f32 min_hz = 0.0f;
        f32 max_hz = 0.0f;
        FrequencyScale scale = FrequencyScale::Log;
        /// Row-major, row 0 = lowest frequency, values 0..1.
        std::vector<f32> values;
        [[nodiscard]] f32 at(u32 row, u32 column) const { return values[static_cast<usize>(row) * columns + column]; }
        /// Centre frequency of a row.
        [[nodiscard]] f32 frequency_of_row(u32 row) const;
    };

    [[nodiscard]] Spectrogram compute_spectrogram(const SampleBuffer &buffer, u64 start, u64 end, const SpectrogramOptions &options = {});
    /// Colours a spectrogram into `width` x `height` pixels (nearest-neighbour scaling, low frequencies at the bottom).
    [[nodiscard]] Image render_spectrogram(const Spectrogram &spectrogram, u32 width, u32 height, ColorMap map = ColorMap::Magma);
    /// A colour from a map for a value in 0..1 (for legends and custom drawing).
    [[nodiscard]] Color colormap_color(ColorMap map, f32 value);

    // ---- scopes and meters ------------------------------------------------------------------------------------------------------------------

    /// Oscilloscope trace of `samples`, triggered on the first rising zero crossing so periodic signals hold still.
    [[nodiscard]] Image render_oscilloscope(std::span<const f32> samples, u32 width, u32 height, Color trace = 0x6BE675FF, Color background = 0x0B120DFF, bool trigger = true);
    /// Goniometer / vectorscope of a stereo pair: M/S Lissajous plot of the samples, brighter where they pile up.
    [[nodiscard]] Image render_goniometer(std::span<const f32> left, std::span<const f32> right, u32 size, Color trace = 0x6BE675FF, Color background = 0x0B120DFF);
    /// Bars for 0..1 values (spectrum analyser style), one per entry.
    [[nodiscard]] Image render_bars(std::span<const f32> values, u32 width, u32 height, Color bar = 0x4FA3D8FF, Color peak = 0xFFFFFFFF, Color background = 0x11161BFF, u32 gap = 1);
    /// Phase correlation of a stereo pair: +1 mono-compatible, 0 unrelated, -1 out of phase.
    [[nodiscard]] f32 stereo_correlation(std::span<const f32> left, std::span<const f32> right);

    /// Writes an image as a PNG (no filtering, stored deflate: dependency-free, for debugging and exports).
    [[nodiscard]] std::expected<void, UString> write_png(const std::filesystem::path &path, const Image &image);

} // namespace SFT::Audio
