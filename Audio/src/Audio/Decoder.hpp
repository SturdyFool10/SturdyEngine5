#pragma once

#include <Audio/Source.hpp>

#include <Foundation/FileIo.hpp>
#include <Audio/Text.hpp>

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Reading audio files. A `DecoderRegistry` holds codec backends; opening a file probes each backend's recognition of the
/// header (falling back on the file extension) and returns a `StreamDecoder` that produces interleaved float frames at the
/// file's own channel count and rate. Built in: WAV (PCM 8/16/24/32, float, IMA/MS ADPCM), AIFF/AIFC, FLAC, MP3, Ogg Vorbis,
/// and, when the platform provides them, AAC/M4A/ALAC/WMA (Media Foundation on Windows, AudioToolbox on macOS) and Opus
/// (with the optional opusfile build). New formats are a `DecoderBackend` away.
namespace SFT::Audio {

    struct AudioStreamInfo {
        u32 channels = 0;
        u32 sample_rate = 0;
        /// Channel meaning (WAVE_FORMAT_EXTENSIBLE masks, Opus/Vorbis channel mappings, ambisonic tags); `layout.channels == 0`
        /// means "guess from the count".
        ChannelLayoutInfo layout;
        /// Total frames, or 0 when the decoder cannot know (some streams).
        u64 total_frames = 0;
        UString codec;
        /// Cue points and loop region authored in the file (WAV cue/smpl chunks, AIFF markers, Vorbis loop tags).
        std::vector<AudioMarker> markers;
        std::optional<LoopRegion> loop;
        /// Title/artist and friends when the container carries them (lower-case keys).
        std::vector<std::pair<UString, UString>> tags;
    };

    class StreamDecoder {
      public:
        virtual ~StreamDecoder() = default;
        [[nodiscard]] virtual const AudioStreamInfo &info() const = 0;
        /// Decodes up to `frames` interleaved float frames into `out`; returns frames produced, 0 at the end.
        virtual u64 read(f32 *out, u64 frames) = 0;
        /// Jumps to a frame (sample accurate where the codec allows, else to the nearest earlier decodable point).
        virtual bool seek(u64 frame) = 0;
        [[nodiscard]] virtual u64 position() const = 0;
    };

    /// Encoded file contents: a heap copy for small files, a memory mapping for big ones (so a file larger than RAM streams
    /// through the page cache instead of being loaded). Backends only need `data()` and `size()`.
    using EncodedBytes = Foundation::Io::SharedBlob;

    class DecoderBackend {
      public:
        virtual ~DecoderBackend() = default;
        [[nodiscard]] virtual ustr name() const = 0;
        /// Extensions (lower case, with dot) this backend is known for; used as a fallback hint and for file dialogs.
        [[nodiscard]] virtual std::vector<UString> extensions() const = 0;
        /// How sure the backend is that `header` (the first bytes of the file) is its format: 0 = not mine, 100 = certain.
        [[nodiscard]] virtual int probe(std::span<const std::byte> header, const UString &extension) const = 0;
        [[nodiscard]] virtual std::unique_ptr<StreamDecoder> open(EncodedBytes bytes) const = 0;
    };

    class DecoderRegistry {
      public:
        /// A registry with every backend this build provides.
        [[nodiscard]] static DecoderRegistry with_defaults();
        /// The process-wide registry (built once with the defaults); register extra backends on it at startup.
        [[nodiscard]] static DecoderRegistry &global();

        void add(std::unique_ptr<DecoderBackend> backend);
        [[nodiscard]] std::vector<UString> supported_extensions() const;

        /// Opens encoded data; `extension_hint` (like ".ogg", optional) breaks ties between backends.
        [[nodiscard]] std::expected<std::unique_ptr<StreamDecoder>, UString> open(EncodedBytes bytes, const UString &extension_hint = {}) const;
        [[nodiscard]] std::expected<std::unique_ptr<StreamDecoder>, UString> open_file(const std::filesystem::path &path) const;

      private:
        std::vector<std::unique_ptr<DecoderBackend>> backends_;
    };

    /// Reads a whole file into a shareable buffer (for sound effects). Markers and loop points come along.
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> load_sound_file(const std::filesystem::path &path);
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> load_sound_memory(EncodedBytes bytes, const UString &extension_hint = {});
    /// Fully decodes any decoder (used by the two above).
    [[nodiscard]] std::expected<std::shared_ptr<SampleBuffer>, UString> decode_all(StreamDecoder &decoder);

    /// Opens a file's bytes (shared helper for backends and callers); `hint` says how the file will be read.
    [[nodiscard]] std::expected<EncodedBytes, UString> read_file_bytes(const std::filesystem::path &path,
                                                                           Foundation::Io::AccessHint hint = Foundation::Io::AccessHint::Random);

    /// Writes `buffer` as a RIFF/WAVE file (16-bit PCM or 32-bit float), with its markers as cue points. A convenience over
    /// `encode_buffer` in Encode.hpp, which writes every format with full control.
    [[nodiscard]] std::expected<void, UString> write_wav(const std::filesystem::path &path, const SampleBuffer &buffer, bool float32 = false);

    // ---- factory hooks for the individual backends (defined in their own files) -------------------------------------
    [[nodiscard]] std::unique_ptr<DecoderBackend> make_miniaudio_backend();
    [[nodiscard]] std::unique_ptr<DecoderBackend> make_aiff_backend();
    [[nodiscard]] std::unique_ptr<DecoderBackend> make_vorbis_backend();
    /// Null when the build has no opusfile.
    [[nodiscard]] std::unique_ptr<DecoderBackend> make_opus_backend();
    /// Null on platforms without a system codec framework.
    [[nodiscard]] std::unique_ptr<DecoderBackend> make_platform_backend();

    /// Parses RIFF/WAVE chunks for cue points (`cue ` + `LIST adtl labl`) and the `smpl` loop, adding them to `info`.
    void parse_wav_metadata(std::span<const std::byte> data, AudioStreamInfo &info);

} // namespace SFT::Audio
