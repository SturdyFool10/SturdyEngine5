#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Decoder.hpp>
#include <Audio/Source.hpp>

#include <Audio/Text.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// Writing audio. One description of what you want (`EncodeOptions`) covers every format, and the same call writes a
/// decoded buffer, a live recording, or a transcode of a file too big to hold in memory:
///
///     encode_buffer("out.flac", buffer, {.format = AudioFormat::Flac, .flac_compression = 8});
///     transcode("take.wav", "take.opus", {.format = AudioFormat::Opus, .bitrate_bps = 96000});
///     auto encoder = open_encoder("mic.w64"...);   // streaming: push blocks as they arrive, `finish()` at the end
///
/// Formats are `EncoderBackend`s in a registry, like the decoders, so a project can add its own (or an OS/third-party
/// codec) with `EncoderRegistry::global().add(...)`.
namespace SFT::Audio {

    enum class AudioFormat : u8 {
        Wav,        ///< RIFF/WAVE; switches to RF64 beyond 4 GB; any channel count (WAVE_FORMAT_EXTENSIBLE)
        Wave64,     ///< Sony Wave64: WAV with 64-bit sizes, for very long multi-channel recordings
        Aiff,       ///< AIFF / AIFF-C (float)
        Flac,
        OggVorbis,
        Opus,       ///< Ogg Opus, mono to 255 channels
        Mp3,        ///< needs libmp3lame at run time (loaded dynamically, never linked) or an OS encoder
        Aac,        ///< M4A through the OS encoder (Media Foundation, AudioToolbox)
        Alac,       ///< Apple Lossless through AudioToolbox
        RawPcm,     ///< headerless samples in `sample_format`, interleaved, little endian
    };

    [[nodiscard]] ustr format_name(AudioFormat format) noexcept;
    /// Preferred file extension with the dot (".flac").
    [[nodiscard]] ustr format_extension(AudioFormat format) noexcept;
    /// Guesses a format from a file extension (".opus", "ogg", ".m4a"...); nullopt when unknown.
    [[nodiscard]] std::optional<AudioFormat> format_from_extension(const UString &extension) noexcept;

    /// How PCM samples are stored (WAV, AIFF, Wave64, raw, and the input depth of FLAC).
    enum class SampleFormat : u8 { UInt8, Int16, Int24, Int32, Float32, Float64 };

    enum class OpusApplication : u8 {
        Audio,     ///< music and general sound
        Voice,     ///< speech: better at low bitrates
        LowDelay,  ///< minimum latency (no look-ahead)
    };

    struct EncodeOptions {
        AudioFormat format = AudioFormat::Wav;

        // ---- PCM family
        SampleFormat sample_format = SampleFormat::Int16;
        /// Add triangular dither when reducing to 16 bits or fewer (hides quantisation distortion at low levels).
        bool dither = true;

        // ---- lossy and compressed
        /// Target average bitrate in bits per second; 0 lets the codec choose from `quality` or its own default.
        u32 bitrate_bps = 0;
        /// 0..1, higher is better; negative = unset. For Vorbis this is the VBR quality (-0.1..1 scaled from 0..1),
        /// for MP3 the VBR level, for Opus it picks a default bitrate when `bitrate_bps` is 0.
        f32 quality = -1.0f;
        /// Variable bitrate (the default everywhere it exists); false forces constant bitrate where supported.
        bool variable_bitrate = true;
        /// FLAC effort, 0 (fastest) to 8 (smallest).
        u32 flac_compression = 5;
        OpusApplication opus_application = OpusApplication::Audio;
        /// Opus algorithmic complexity 0..10 (10 = best quality, slowest).
        u32 opus_complexity = 10;
        /// Opus frame length in milliseconds: 2.5, 5, 10, 20, 40 or 60.
        f32 opus_frame_ms = 20.0f;

        // ---- format conversion applied on the way in
        /// Resample to this rate (0 = keep). Opus always runs at 48 kHz internally and is resampled for you.
        u32 sample_rate = 0;
        /// Remix to this many channels with the standard downmix/upmix rules (0 = keep).
        u32 channels = 0;
        /// Linear gain applied before encoding (1 = unchanged); set < 1 to leave headroom for lossy codecs.
        f32 gain = 1.0f;

        // ---- metadata
        /// Title/artist/album/... (lower-case keys: "title", "artist", "album", "comment", "date", "genre", "track").
        std::vector<std::pair<UString, UString>> tags;
        /// Cue points and loop region, written where the format has them (WAV cue/smpl, AIFF markers, Vorbis/Opus/FLAC tags).
        std::vector<AudioMarker> markers;
        std::optional<LoopRegion> loop;
    };

    /// Receives audio a block at a time and writes it. Not thread safe: one writer thread.
    class AudioEncoder {
      public:
        virtual ~AudioEncoder() = default;
        /// Appends `frames` interleaved frames (`channels()` channels at `sample_rate()`), as given to `open_encoder`
        /// (conversion to the options' rate/channels happens inside). False after the first error; see `error()`.
        virtual bool write(const f32 *interleaved, usize frames) = 0;
        /// Finishes the file (flushes the codec, patches headers, moves it into place). The encoder is unusable after.
        [[nodiscard]] virtual std::expected<void, UString> finish() = 0;
        [[nodiscard]] virtual const UString &error() const = 0;
        /// Frames accepted so far (input rate).
        [[nodiscard]] virtual u64 frames_written() const = 0;
    };

    /// What a backend gets once the front end has normalised options (rate/channel conversion is done by the front end).
    struct EncoderConfig {
        std::filesystem::path path;
        u32 channels = 2;
        u32 sample_rate = 48000;
        EncodeOptions options;
        /// Layout of the channels so formats that record it (WAV masks, Opus mapping) can.
        ChannelLayoutInfo layout;
    };

    class EncoderBackend {
      public:
        virtual ~EncoderBackend() = default;
        [[nodiscard]] virtual ustr name() const = 0;
        [[nodiscard]] virtual AudioFormat format() const = 0;
        /// The rate the codec will actually run at when asked for `requested` (Opus: always 48000; MP3: the nearest legal
        /// rate). The front end resamples to it, so `EncoderConfig::sample_rate` is always one the backend accepts.
        [[nodiscard]] virtual u32 constrain_sample_rate(u32 requested) const { return requested; }
        /// Most channels the format can carry.
        [[nodiscard]] virtual u32 max_channels() const { return max_source_channels; }
        [[nodiscard]] virtual std::expected<std::unique_ptr<AudioEncoder>, UString> open(const EncoderConfig &config) const = 0;
    };

    class EncoderRegistry {
      public:
        [[nodiscard]] static EncoderRegistry with_defaults();
        [[nodiscard]] static EncoderRegistry &global();
        void add(std::unique_ptr<EncoderBackend> backend);
        [[nodiscard]] const EncoderBackend *find(AudioFormat format) const;
        /// Formats this build can write.
        [[nodiscard]] std::vector<AudioFormat> available() const;

      private:
        std::vector<std::unique_ptr<EncoderBackend>> backends_;
    };

    /// Opens a streaming encoder writing `path`. `channels`/`sample_rate` describe the frames you will `write`.
    /// `layout` says what the channels are (a file's speaker roles, an ambisonic recording); omitted, it is guessed from the count.
    [[nodiscard]] std::expected<std::unique_ptr<AudioEncoder>, UString> open_encoder(
        const std::filesystem::path &path, u32 channels, u32 sample_rate, const EncodeOptions &options = {},
        const std::optional<ChannelLayoutInfo> &layout = std::nullopt);

    /// Encodes a whole decoded buffer. Its markers and loop region are written too unless `options` carries its own.
    [[nodiscard]] std::expected<void, UString> encode_buffer(const std::filesystem::path &path, const SampleBuffer &buffer, EncodeOptions options = {});

    /// Called with progress in [0, 1] (0 when the length is unknown); return false to cancel.
    using ProgressCallback = std::function<bool(f32 fraction)>;

    /// Decodes `input` and encodes it as `output` in blocks, so memory use is constant however long the file is. Tags,
    /// markers and the loop region of the source are carried over unless `options` provides its own.
    [[nodiscard]] std::expected<void, UString> transcode(const std::filesystem::path &input, const std::filesystem::path &output,
                                                              EncodeOptions options = {}, const ProgressCallback &progress = {});
    /// The same from any open decoder.
    [[nodiscard]] std::expected<void, UString> transcode(StreamDecoder &decoder, const std::filesystem::path &output,
                                                              EncodeOptions options = {}, const ProgressCallback &progress = {});

    // ---- backends (defined in their own files; null when the build left the codec out) --------------------------------------------
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_wav_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_wave64_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_aiff_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_raw_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_flac_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_vorbis_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_opus_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_mp3_encoder();
    /// AAC/ALAC through the OS; null on platforms without a system encoder.
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_platform_aac_encoder();
    [[nodiscard]] std::unique_ptr<EncoderBackend> make_platform_alac_encoder();

} // namespace SFT::Audio
