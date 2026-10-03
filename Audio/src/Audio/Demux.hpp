#pragma once

#include <Audio/Decoder.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

/// Container demultiplexing: Matroska / WebM (`.mkv .mka .webm`) and MP4 / ISO base media (`.mp4 .m4a .m4b .mov`, progressive and
/// fragmented) opened from memory or a mapped file. The demuxer reads only the structure and hands out the audio packets (views
/// into the file, never copies), so a movie larger than RAM costs address space and a small index, and a video player can feed
/// the packets to its own decoder and a `MediaAudioStream` with the right timestamps.
///
/// Packets are not decoded here. `make_container_backend()` (a `DecoderBackend`) decodes the codecs the engine can handle
/// itself (Opus, PCM, and FLAC/MP3 through the stream decoders); anything else, such as AAC on a build with no system decoder,
/// is reported by name so the caller can bring a decoder.
namespace SFT::Audio {

    enum class ContainerKind : u8 { Matroska, Mp4 };

    struct DemuxTrack {
        u32 id = 0;
        /// "opus", "vorbis", "flac", "aac", "mp3", "mp2", "ac3", "eac3", "alac", "pcm_s16le", "pcm_s24le", "pcm_s32le", "pcm_f32le",
        /// "pcm_s16be", ... or the raw codec identifier when unknown.
        UString codec;
        u32 channels = 0;
        u32 sample_rate = 0;
        u32 bit_depth = 0;
        /// Codec-specific setup: OpusHead for Opus, AudioSpecificConfig for AAC, STREAMINFO for FLAC, the three header packets
        /// (Xiph-laced) for Vorbis.
        std::vector<u8> codec_private;
        f64 duration_seconds = 0.0;
        u64 packet_count = 0;
        UString language;
    };

    struct DemuxPacket {
        u32 track = 0;
        f64 pts_seconds = 0.0;
        f64 duration_seconds = 0.0;
        bool keyframe = true;
        std::span<const std::byte> data;
    };

    class Demuxer {
      public:
        virtual ~Demuxer() = default;
        [[nodiscard]] virtual ContainerKind kind() const noexcept = 0;
        [[nodiscard]] virtual std::span<const DemuxTrack> tracks() const noexcept = 0;
        [[nodiscard]] virtual f64 duration_seconds() const noexcept = 0;
        /// The next packet of any audio track, in file order (which is timestamp order for audio); nothing at the end.
        [[nodiscard]] virtual std::optional<DemuxPacket> next() = 0;
        /// The next packet of one track.
        [[nodiscard]] virtual std::optional<DemuxPacket> next(u32 track) = 0;
        /// Repositions so that `next` returns the last packet at or before `seconds` (the first packet when before it).
        virtual void seek(f64 seconds) = 0;
        [[nodiscard]] virtual u64 packet_count() const noexcept = 0;

        /// Opens a container, choosing Matroska or MP4 from the bytes.
        [[nodiscard]] static std::expected<std::unique_ptr<Demuxer>, UString> open(EncodedBytes bytes);
        [[nodiscard]] static std::expected<std::unique_ptr<Demuxer>, UString> open_file(const std::filesystem::path &path);
        /// Whether `header` (the first bytes) looks like a container this demuxer reads.
        [[nodiscard]] static std::optional<ContainerKind> sniff(std::span<const std::byte> header) noexcept;
    };

    /// A packet reference: where the data is and when it plays. The shared index both formats fill.
    struct PacketRef {
        u64 offset = 0;
        u32 size = 0;
        u32 track = 0;
        i64 pts_ticks = 0;
        u32 duration_ticks = 0;
        bool keyframe = true;
    };

    /// What a format parser produces; `IndexedDemuxer` turns it into a `Demuxer` (seeking, per-track reads, timestamps).
    struct ContainerIndex {
        ContainerKind kind = ContainerKind::Matroska;
        std::vector<DemuxTrack> tracks;
        /// Seconds per tick for each track (Matroska: one timescale for all; MP4: each track's media timescale).
        std::vector<f64> tick_seconds;
        std::vector<PacketRef> packets;
        f64 duration_seconds = 0.0;
    };

    [[nodiscard]] std::expected<ContainerIndex, UString> parse_matroska(std::span<const std::byte> file);
    [[nodiscard]] std::expected<ContainerIndex, UString> parse_mp4(std::span<const std::byte> file);
    [[nodiscard]] std::unique_ptr<Demuxer> make_indexed_demuxer(EncodedBytes bytes, ContainerIndex index);

    [[nodiscard]] std::unique_ptr<DecoderBackend> make_container_backend();

} // namespace SFT::Audio
